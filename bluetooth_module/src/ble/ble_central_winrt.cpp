// BLE backend for Windows, built on the WinRT Bluetooth APIs through C++/WinRT.
//
// Two WinRT particulars shape the code. First, scanning and connecting are separate worlds: the
// advertisement watcher only reports addresses, and a connection is formed implicitly the moment
// GATT services are requested from a BluetoothLEDevice. Second, every asynchronous operation
// returns an IAsyncOperation whose completion has to be waited on, which suits our synchronous
// interface -- but such a wait must never happen on an STA thread.
//
// That second point is why this file owns an apartment thread. C++/WinRT asserts
// (WINRT_ASSERT(!is_sta_thread()) in windows.foundation.h) and then silently stalls the message
// pump when a blocking wait runs on an STA thread -- and a module does not get to choose its
// callers. The host application may well have initialised COM as an STA (a Flutter Windows app
// does exactly that, CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED) in its runner), other
// callers arrive on threads that never initialised COM at all, and openDAQ invokes property
// handlers from arbitrary scheduler threads. Rather than trying to fix up whatever apartment each
// caller happens to be in, the backend keeps one MTA thread of its own and marshals every WinRT
// call onto it -- activation, waits, event registration and revocation, and the release of every
// WinRT object. Nothing above this file has to know or care what apartment it is calling from.
//
// The one thing deliberately NOT marshalled is the event handlers themselves. WinRT invokes them
// on its own threadpool threads, which are already MTA; bouncing them onto the apartment thread
// would deadlock a notification that arrives while a read is in flight. Because they run outside
// the apartment, no delegate here may capture a raw `this`: WinRT does not promise that revoking
// an event waits for handlers already running, and Microsoft documents that a watcher's Received
// can fire even after Stop(). Everything a handler touches therefore lives in a shared_ptr state
// block (ConnectionState / ScanState) that the delegate holds a reference to, exactly as the
// CoreBluetooth backend does with its own `struct SharedState`.

#include <bluetooth_module/ble/ble_central.h>

#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace opendaq_bt::ble
{

namespace
{

using namespace std::chrono;
using namespace winrt;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage::Streams;

/*!
 * How long to wait on the adapter/radio queries in isAvailable(). Short on purpose: this runs on
 * every getAvailableDevices() and every createDevice(), so a wedged radio stack must not be able
 * to stall discovery for the whole instance.
 */
constexpr milliseconds AdapterQueryTimeout{3000};

/*!
 * Ceiling for a single GATT read/write/subscribe. IConnection has no timeout parameter of its
 * own -- these run inside openDAQ property handlers, and blocking one of those forever is far
 * worse than failing it -- so the backend supplies one.
 */
constexpr milliseconds GattOperationTimeout{30000};

/*!
 * Ceiling for the connected-device enumeration in getDiscovered(). Discovery is a synchronous
 * openDAQ call, so this has to be short even when the enumeration is being slow.
 */
constexpr milliseconds ConnectedQueryTimeout{2000};

/*!
 * How long a connected-device enumeration is reused before being redone. getDiscovered() is polled
 * several times a second by an application waiting for a scan to fill up, and FindAllAsync plus a
 * FromIdAsync per device is far too heavy to repeat at that rate for a set that changes rarely.
 */
constexpr milliseconds ConnectedQueryInterval{2000};

/*!
 * A dedicated multi-threaded apartment, and the queue used to get work onto it.
 *
 * Every WinRT call in this file runs inside run() for two reasons:
 *
 *   - Blocking waits. See the file header: our callers may be STA or in no apartment at all, and
 *     both are fatal for a blocking wait on an IAsyncOperation.
 *   - Object lifetime. AddRef/Release on a WinRT object from a thread that is in no apartment is
 *     the same category of mistake as calling from one, so construction, copying and destruction
 *     of WinRT types all belong here too -- which is why the destructors below marshal their
 *     teardown rather than just letting members fall out of scope.
 */
class Apartment
{
public:
    Apartment()
    {
        std::promise<void> ready;
        auto started = ready.get_future();

        worker = std::thread([this, &ready] {
            try
            {
                init_apartment(apartment_type::multi_threaded);
            }
            catch (...)
            {
                ready.set_exception(std::current_exception());
                return;
            }

            ready.set_value();
            pump();
            uninit_apartment();
        });

        workerId = worker.get_id();

        try
        {
            started.get();
        }
        catch (const hresult_error& e)
        {
            worker.join();
            throw BleError("Cannot create a multi-threaded COM apartment for Bluetooth: " +
                           to_string(e.message()));
        }
    }

    Apartment(const Apartment&) = delete;
    Apartment& operator=(const Apartment&) = delete;

    ~Apartment()
    {
        {
            std::scoped_lock guard(sync);
            stopping = true;
        }
        wake.notify_all();

        if (worker.joinable())
            worker.join();
    }

    /*!
     * Runs @p f on the apartment thread and returns its result, rethrowing whatever it threw.
     *
     * This is also the single choke point where WinRT errors are translated. winrt::hresult_error
     * does not derive from std::exception, so one escaping from here would sail straight past
     * every catch(const std::exception&) between this file and the FFI boundary and take the
     * process down. Translating it in one place means no call site can forget to.
     */
    template <typename F>
    auto run(F&& f) -> decltype(f())
    {
        using Result = decltype(f());

        // Re-entrant call from the apartment thread itself -- a teardown path reached from inside
        // another task. Running inline is both correct (we are already in the apartment) and the
        // only way not to deadlock waiting on ourselves.
        if (std::this_thread::get_id() == workerId)
            return f();

        std::packaged_task<Result()> task(std::forward<F>(f));
        auto result = task.get_future();

        {
            std::scoped_lock guard(sync);
            if (stopping)
                throw BleError("The Bluetooth apartment thread is shutting down.");

            // The task is a local of this frame, and we block on its future below, so referencing
            // it from the queued closure is safe for exactly as long as it needs to be.
            queue.emplace_back([&task] { task(); });
        }
        wake.notify_one();

        try
        {
            return result.get();
        }
        catch (const hresult_error& e)
        {
            throw BleError(to_string(e.message()));
        }
    }

private:
    void pump()
    {
        for (;;)
        {
            std::function<void()> task;
            {
                std::unique_lock lock(sync);
                wake.wait(lock, [this] { return stopping || !queue.empty(); });

                // Drain before leaving: a task already queued has a caller blocked on its future.
                if (queue.empty())
                    return;

                task = std::move(queue.front());
                queue.pop_front();
            }

            task();
        }
    }

    std::thread worker;
    std::thread::id workerId;

    std::mutex sync;
    std::condition_variable wake;
    std::deque<std::function<void()>> queue;
    bool stopping = false;
};

/*!
 * Waits for a WinRT async operation, turning the timeout into a BleError instead of blocking
 * forever. Must be called from the apartment thread.
 */
template <typename Async>
auto awaitOp(const Async& operation, milliseconds timeout, const std::string& what)
{
    if (operation.wait_for(duration_cast<TimeSpan>(timeout)) != AsyncStatus::Completed)
    {
        operation.Cancel();
        throw BleError("Timed out " + what + ".");
    }

    return operation.GetResults();
}

std::string formatAddress(uint64_t address)
{
    char buffer[18];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%02X:%02X:%02X:%02X:%02X:%02X",
                  static_cast<unsigned>((address >> 40) & 0xFF),
                  static_cast<unsigned>((address >> 32) & 0xFF),
                  static_cast<unsigned>((address >> 24) & 0xFF),
                  static_cast<unsigned>((address >> 16) & 0xFF),
                  static_cast<unsigned>((address >> 8) & 0xFF),
                  static_cast<unsigned>(address & 0xFF));

    return buffer;
}

/*! "AA:BB:CC:DD:EE:FF" back to the 48-bit value WinRT wants. */
uint64_t parseAddress(const std::string& address)
{
    uint64_t value = 0;
    int digits = 0;

    for (const char c : address)
    {
        if (c == ':' || c == '-')
            continue;

        value <<= 4;
        if (c >= '0' && c <= '9')
            value |= static_cast<uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
            value |= static_cast<uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            value |= static_cast<uint64_t>(c - 'A' + 10);
        else
            throw BleError("Not a Bluetooth address: " + address);

        ++digits;
    }

    // Without this a 36-character macOS-style identifier would shift its way to a plausible-
    // looking 48-bit number and fail much later as "device not found".
    if (digits != 12)
        throw BleError("Not a Bluetooth address: " + address + " (expected 12 hex digits, got " +
                       std::to_string(digits) + ")");

    return value;
}

std::string toString(const hstring& value)
{
    return to_string(value);
}

std::string uuidToString(const guid& id)
{
    char buffer[37];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  static_cast<unsigned>(id.Data1),
                  static_cast<unsigned>(id.Data2),
                  static_cast<unsigned>(id.Data3),
                  static_cast<unsigned>(id.Data4[0]),
                  static_cast<unsigned>(id.Data4[1]),
                  static_cast<unsigned>(id.Data4[2]),
                  static_cast<unsigned>(id.Data4[3]),
                  static_cast<unsigned>(id.Data4[4]),
                  static_cast<unsigned>(id.Data4[5]),
                  static_cast<unsigned>(id.Data4[6]),
                  static_cast<unsigned>(id.Data4[7]));

    return buffer;
}

uint32_t mapProperties(GattCharacteristicProperties properties)
{
    uint32_t result = CharPropNone;

    const auto has = [&](GattCharacteristicProperties flag) {
        return (properties & flag) == flag;
    };

    if (has(GattCharacteristicProperties::Read))
        result |= CharPropRead;
    if (has(GattCharacteristicProperties::Write))
        result |= CharPropWrite;
    if (has(GattCharacteristicProperties::WriteWithoutResponse))
        result |= CharPropWriteWithoutResponse;
    if (has(GattCharacteristicProperties::Notify))
        result |= CharPropNotify;
    if (has(GattCharacteristicProperties::Indicate))
        result |= CharPropIndicate;
    if (has(GattCharacteristicProperties::Broadcast))
        result |= CharPropBroadcast;
    if (has(GattCharacteristicProperties::AuthenticatedSignedWrites))
        result |= CharPropAuthenticatedSignedWrites;
    if (has(GattCharacteristicProperties::ExtendedProperties))
        result |= CharPropExtendedProperties;

    return result;
}

std::vector<uint8_t> bufferToBytes(const IBuffer& buffer)
{
    if (buffer == nullptr)
        return {};

    const auto reader = DataReader::FromBuffer(buffer);
    std::vector<uint8_t> bytes(buffer.Length());
    if (!bytes.empty())
        reader.ReadBytes(array_view<uint8_t>(bytes));

    return bytes;
}

IBuffer bytesToBuffer(const std::vector<uint8_t>& bytes)
{
    DataWriter writer;
    writer.WriteBytes(array_view<const uint8_t>(bytes));
    return writer.DetachBuffer();
}

/*!
 * The part of a connection that WinRT event handlers touch, held by shared_ptr so a handler
 * still running on a threadpool thread cannot outlive it.
 *
 * Revoking a WinRT event is not documented to block until in-flight handlers have returned, so a
 * delegate capturing a raw `this` would be a use-after-free the moment teardown races a
 * notification. The CoreBluetooth backend solves the same problem the same way -- see
 * `struct SharedState` in ble_central_corebluetooth.mm.
 */
struct ConnectionState
{
    std::mutex sync;
    std::function<void()> disconnectCallback;
    std::map<std::string, NotificationCallback> notificationCallbacks;
    std::atomic<bool> connected{true};
};

class WinRtConnection final : public IConnection
{
public:
    WinRtConnection(std::shared_ptr<Apartment> apartment,
                    BluetoothLEDevice device,
                    GattSession session,
                    std::vector<GattDeviceService> gattServices,
                    std::vector<Service> services,
                    std::map<std::string, GattCharacteristic> characteristics,
                    uint16_t mtuSize)
        : apartment(std::move(apartment))
        , state(std::make_shared<ConnectionState>())
        , device(std::move(device))
        , session(std::move(session))
        , gattServices(std::move(gattServices))
        , resolvedServices(std::move(services))
        , characteristicsByHandle(std::move(characteristics))
        , mtuSize(mtuSize)
    {
        // Copied into a local rather than captured as `[state = this->state]`: MSVC resolves
        // `this` inside a nested lambda's init-capture against the enclosing lambda's closure
        // type instead of the enclosing object, and rejects it. (The `this->` on the members is
        // separately necessary -- the constructor parameters shadow them.)
        auto connectionState = this->state;

        connectionToken = this->apartment->run([this, connectionState] {
            return this->device.ConnectionStatusChanged(
                [connectionState](const BluetoothLEDevice& sender, const IInspectable&) {
                    if (sender.ConnectionStatus() != BluetoothConnectionStatus::Disconnected)
                        return;

                    connectionState->connected = false;

                    std::function<void()> callback;
                    {
                        std::scoped_lock guard(connectionState->sync);
                        callback = connectionState->disconnectCallback;
                    }
                    if (callback)
                        callback();
                });
        });
    }

    ~WinRtConnection() override
    {
        // Drop the user callbacks first: a handler that fires during teardown then finds nothing
        // to call, instead of reaching into a half-destroyed object.
        {
            std::scoped_lock guard(state->sync);
            state->disconnectCallback = nullptr;
            state->notificationCallbacks.clear();
        }

        try
        {
            // Revoking an event blocks until in-flight handlers return, and every WinRT object
            // here has to be released from inside the apartment, so the whole teardown goes over
            // there in one hop.
            apartment->run([this] {
                for (const auto& [handle, token] : valueChangedTokens)
                {
                    const auto it = characteristicsByHandle.find(handle);
                    if (it != characteristicsByHandle.end())
                        it->second.ValueChanged(token);
                }
                valueChangedTokens.clear();

                if (device != nullptr)
                {
                    device.ConnectionStatusChanged(connectionToken);

                    // Releasing every reference to the device object is what actually tears the
                    // link down; the session is what has been holding it up. GattDeviceService
                    // and GattSession are both IClosable, and the services are retained for
                    // exactly this reason -- so that "every reference" is something this code
                    // owns and drops deterministically, rather than whatever the characteristics
                    // happen to keep alive.
                    characteristicsByHandle.clear();

                    for (auto& gattService : gattServices)
                        gattService.Close();
                    gattServices.clear();

                    if (session != nullptr)
                    {
                        session.Close();
                        session = nullptr;
                    }

                    device.Close();
                    device = nullptr;
                }
            });
        }
        catch (const BleError&)
        {
            // A destructor must not throw, and the apartment is going away regardless.
        }
    }

    bool isConnected() const override { return state->connected; }
    const std::vector<Service>& services() const override { return resolvedServices; }
    uint16_t mtu() const override { return mtuSize; }

    std::vector<uint8_t> read(const std::string& handle) override
    {
        return apartment->run([&] {
            const auto& characteristic = lookup(handle);

            const auto result = awaitOp(characteristic.ReadValueAsync(BluetoothCacheMode::Uncached),
                                        GattOperationTimeout,
                                        "reading " + handle);

            if (result.Status() != GattCommunicationStatus::Success)
                throw BleError("Reading " + handle + " failed with GATT status " +
                               std::to_string(static_cast<int>(result.Status())));

            return bufferToBytes(result.Value());
        });
    }

    void write(const std::string& handle,
               const std::vector<uint8_t>& data,
               bool withResponse) override
    {
        apartment->run([&] {
            const auto& characteristic = lookup(handle);

            const auto option = withResponse ? GattWriteOption::WriteWithResponse
                                             : GattWriteOption::WriteWithoutResponse;

            const auto status = awaitOp(characteristic.WriteValueAsync(bytesToBuffer(data), option),
                                        GattOperationTimeout,
                                        "writing " + handle);

            if (status != GattCommunicationStatus::Success)
                throw BleError("Writing " + handle + " failed with GATT status " +
                               std::to_string(static_cast<int>(status)));
        });
    }

    void subscribe(const std::string& handle, NotificationCallback callback) override
    {
        {
            std::scoped_lock guard(state->sync);
            state->notificationCallbacks[handle] = std::move(callback);
        }

        // See the constructor: the nested delegate must capture a plain local, not `this->state`.
        auto connectionState = state;

        try
        {
            apartment->run([&] {
                const auto& characteristic = lookup(handle);

                // Indicate and Notify are distinct on the wire; pick whichever the characteristic
                // advertises, preferring Notify since it needs no acknowledgement.
                const auto properties = characteristic.CharacteristicProperties();
                const auto notifies = (properties & GattCharacteristicProperties::Notify) ==
                                      GattCharacteristicProperties::Notify;
                const auto indicates = (properties & GattCharacteristicProperties::Indicate) ==
                                       GattCharacteristicProperties::Indicate;

                if (!notifies && !indicates)
                    throw BleError("Characteristic " + handle +
                                   " supports neither notifications nor indications.");

                const auto registeredHere =
                    valueChangedTokens.find(handle) == valueChangedTokens.end();

                if (registeredHere)
                {
                    valueChangedTokens[handle] = characteristic.ValueChanged(
                        [connectionState, handle](const GattCharacteristic&,
                                                  const GattValueChangedEventArgs& args) {
                            NotificationCallback cb;
                            {
                                std::scoped_lock guard(connectionState->sync);
                                const auto it = connectionState->notificationCallbacks.find(handle);
                                if (it == connectionState->notificationCallbacks.end())
                                    return;
                                cb = it->second;
                            }
                            if (cb)
                                cb(bufferToBytes(args.CharacteristicValue()));
                        });
                }

                // Anything below that throws has to undo that registration. Leaving it behind
                // would make a later retry take the already-registered path above and never
                // re-register, so the subscription would look live and deliver nothing.
                const auto revokeOnFailure = [&] {
                    if (!registeredHere)
                        return;

                    const auto it = valueChangedTokens.find(handle);
                    if (it != valueChangedTokens.end())
                    {
                        characteristic.ValueChanged(it->second);
                        valueChangedTokens.erase(it);
                    }
                };

                // Writing the Client Characteristic Configuration Descriptor is what actually asks
                // the peripheral to start sending.
                const auto value = notifies
                                       ? GattClientCharacteristicConfigurationDescriptorValue::Notify
                                       : GattClientCharacteristicConfigurationDescriptorValue::Indicate;

                GattCommunicationStatus status{};
                try
                {
                    status = awaitOp(
                        characteristic.WriteClientCharacteristicConfigurationDescriptorAsync(value),
                        GattOperationTimeout,
                        "subscribing to " + handle);
                }
                catch (...)
                {
                    revokeOnFailure();
                    throw;
                }

                if (status != GattCommunicationStatus::Success)
                {
                    revokeOnFailure();
                    throw BleError("Cannot subscribe to " + handle + " (GATT status " +
                                   std::to_string(static_cast<int>(status)) + ")");
                }
            });
        }
        catch (...)
        {
            std::scoped_lock guard(state->sync);
            state->notificationCallbacks.erase(handle);
            throw;
        }
    }

    void unsubscribe(const std::string& handle) override
    {
        {
            std::scoped_lock guard(state->sync);
            state->notificationCallbacks.erase(handle);
        }

        apartment->run([&] {
            const auto it = characteristicsByHandle.find(handle);
            if (it == characteristicsByHandle.end())
                return;

            const auto tokenIt = valueChangedTokens.find(handle);
            if (tokenIt != valueChangedTokens.end())
            {
                it->second.ValueChanged(tokenIt->second);
                valueChangedTokens.erase(tokenIt);
            }

            awaitOp(it->second.WriteClientCharacteristicConfigurationDescriptorAsync(
                        GattClientCharacteristicConfigurationDescriptorValue::None),
                    GattOperationTimeout,
                    "unsubscribing from " + handle);
        });
    }

    void setDisconnectCallback(std::function<void()> callback) override
    {
        std::scoped_lock guard(state->sync);
        state->disconnectCallback = std::move(callback);
    }

private:
    /*!
     * Apartment-thread only -- which is also what makes characteristicsByHandle and
     * valueChangedTokens safe to touch without a lock: every path that reads or writes them is
     * inside an apartment->run().
     */
    const GattCharacteristic& lookup(const std::string& handle)
    {
        const auto it = characteristicsByHandle.find(handle);
        if (it == characteristicsByHandle.end())
            throw BleError("No such characteristic: " + handle);

        if (!state->connected)
            throw BleError("Device is disconnected.");

        return it->second;
    }

    // Declared first so it outlives every WinRT member below it: those are released through it.
    std::shared_ptr<Apartment> apartment;

    // Everything a WinRT event handler touches. Shared rather than owned, so a handler still in
    // flight during teardown keeps it alive.
    std::shared_ptr<ConnectionState> state;

    BluetoothLEDevice device{nullptr};
    GattSession session{nullptr};
    // Retained purely for their lifetime: the services own the characteristics, and closing them
    // in the destructor is what makes the disconnect deterministic.
    std::vector<GattDeviceService> gattServices;
    event_token connectionToken{};
    std::vector<Service> resolvedServices;
    std::map<std::string, GattCharacteristic> characteristicsByHandle;
    std::map<std::string, event_token> valueChangedTokens;
    uint16_t mtuSize;
};

/*!
 * The scan results, held by shared_ptr for the same reason as ConnectionState above: WinRT does
 * not promise that revoking BluetoothLEAdvertisementWatcher::Received waits for handlers already
 * running, and Microsoft explicitly documents that Received can still fire after Stop(). A
 * delegate capturing a raw `this` would therefore be a use-after-free against a WinRtCentral that
 * has already gone.
 */
struct ScanState
{
    std::mutex sync;

    // Never evicted here on purpose -- ScanCache one layer up owns staleness (DeviceTimeoutMs),
    // and it needs to see everything the radio saw to make that call.
    std::map<std::string, Advertisement> discovered;
};

/*! Called on a WinRT threadpool thread, never on the apartment thread. */
void mergeAdvertisement(ScanState& state, const BluetoothLEAdvertisementReceivedEventArgs& args)
{
    Advertisement advertisement;
    advertisement.id = formatAddress(args.BluetoothAddress());
    advertisement.rssi = args.RawSignalStrengthInDBm();
    advertisement.lastSeen = steady_clock::now();
    advertisement.name = toString(args.Advertisement().LocalName());

    for (const auto& uuid : args.Advertisement().ServiceUuids())
        advertisement.serviceUuids.push_back(uuidToString(uuid));

    for (const auto& section : args.Advertisement().ManufacturerData())
        advertisement.manufacturerData[section.CompanyId()] = bufferToBytes(section.Data());

    std::scoped_lock guard(state.sync);

    // Successive advertisements from one peripheral carry different fragments -- the name often
    // arrives only in the scan response -- so merge rather than replace.
    auto& stored = state.discovered[advertisement.id];
    if (advertisement.name.empty())
        advertisement.name = stored.name;
    if (advertisement.serviceUuids.empty())
        advertisement.serviceUuids = stored.serviceUuids;
    if (advertisement.manufacturerData.empty())
        advertisement.manufacturerData = stored.manufacturerData;

    stored = std::move(advertisement);
}

class WinRtCentral final : public ICentral
{
public:
    WinRtCentral()
        : apartment(std::make_shared<Apartment>())
        , scanState(std::make_shared<ScanState>())
    {
    }

    ~WinRtCentral() override
    {
        try
        {
            stopScan();
        }
        catch (const BleError&)
        {
            // Nothing useful to do from a destructor; the apartment is going away anyway.
        }
    }

    bool isAvailable(std::string& reason) override
    {
        try
        {
            return apartment->run([&] {
                const auto adapter = awaitOp(BluetoothAdapter::GetDefaultAsync(),
                                             AdapterQueryTimeout,
                                             "querying the Bluetooth adapter");
                if (adapter == nullptr)
                {
                    reason = "no Bluetooth adapter was found";
                    return false;
                }

                if (!adapter.IsLowEnergySupported())
                {
                    reason = "the Bluetooth adapter does not support Bluetooth Low Energy";
                    return false;
                }

                const auto radio = awaitOp(adapter.GetRadioAsync(),
                                           AdapterQueryTimeout,
                                           "querying the Bluetooth radio");
                if (radio != nullptr &&
                    radio.State() != winrt::Windows::Devices::Radios::RadioState::On)
                {
                    reason = "the Bluetooth radio is turned off";
                    return false;
                }

                return true;
            });
        }
        catch (const BleError& e)
        {
            // isAvailable reports through `reason` rather than throwing -- a host with no adapter
            // is a normal state, not an error.
            reason = e.what();
            return false;
        }
    }

    void startScan() override
    {
        std::scoped_lock guard(scanSync);
        if (scanning)
            return;

        // See WinRtConnection's constructor for why this is a local rather than a `this->`
        // init-capture in the nested delegate below.
        auto state = scanState;

        apartment->run([this, state] {
            watcher = BluetoothLEAdvertisementWatcher();
            // Active scanning solicits scan responses, which is where most peripherals put their
            // name.
            watcher.ScanningMode(BluetoothLEScanningMode::Active);

            receivedToken = watcher.Received(
                [state](const BluetoothLEAdvertisementWatcher&,
                        const BluetoothLEAdvertisementReceivedEventArgs& args) {
                    mergeAdvertisement(*state, args);
                });

            watcher.Start();
        });

        scanning = true;
    }

    void stopScan() override
    {
        std::scoped_lock guard(scanSync);
        if (!scanning)
            return;

        scanning = false;

        apartment->run([this] {
            if (watcher == nullptr)
                return;

            // Two hazards here, both handled. If revoking blocks until in-flight Received
            // handlers return, that is safe because mergeAdvertisement only ever takes
            // ScanState::sync, never scanSync -- the lock this call is holding. And if revoking
            // does NOT block (WinRT does not promise it does), the handler is still safe because
            // it holds its own shared_ptr to the state it touches.
            watcher.Received(receivedToken);
            watcher.Stop();
            watcher = nullptr;
        });
    }

    bool isScanning() const override { return scanning; }

    std::vector<Advertisement> getDiscovered() override
    {
        // Two sources, because a peripheral connected to this host stops advertising: the watcher
        // will never see it again, and it is the one peripheral we are certain we can reach.
        auto linked = connectedPeripherals();

        std::scoped_lock guard(scanState->sync);

        std::vector<Advertisement> result;
        result.reserve(scanState->discovered.size() + linked.size());

        for (const auto& [id, advertisement] : scanState->discovered)
        {
            auto merged = advertisement;

            const auto it = linked.find(id);
            if (it != linked.end())
            {
                // The watcher's reading and manufacturer data are real and worth keeping; the
                // connected entry contributes the flag and the resolved service list, which is
                // more complete than anything that fits in an advertisement.
                merged.connected = true;
                if (!it->second.serviceUuids.empty())
                    merged.serviceUuids = it->second.serviceUuids;
                if (merged.name.empty())
                    merged.name = it->second.name;
                linked.erase(it);
            }

            result.push_back(std::move(merged));
        }

        for (auto& [id, advertisement] : linked)
            result.push_back(std::move(advertisement));

        return result;
    }

    std::unique_ptr<IConnection> connect(const std::string& id, milliseconds timeout) override
    {
        std::string reason;
        if (!isAvailable(reason))
            throw BleError("Cannot connect: " + reason);

        const auto address = parseAddress(id);

        // Everything WinRT is built on the apartment thread and handed back by move, so no
        // refcount is ever touched off it.
        struct Resolved
        {
            BluetoothLEDevice device{nullptr};
            GattSession session{nullptr};
            std::vector<GattDeviceService> gattServices;
            std::vector<Service> services;
            std::map<std::string, GattCharacteristic> characteristicsByHandle;
            uint16_t mtu = 0;
        };

        // The timeout is a per-phase budget rather than one shared across all of them, matching
        // the BlueZ and CoreBluetooth backends.
        auto resolved = apartment->run([&]() -> Resolved {
            Resolved out;

            out.device = awaitOp(BluetoothLEDevice::FromBluetoothAddressAsync(address),
                                 timeout,
                                 "opening Bluetooth device " + id);
            if (out.device == nullptr)
                throw BleError("Bluetooth device " + id +
                               " could not be opened. Run a discovery scan first so Windows can "
                               "find it.");

            // A session with MaintainConnection keeps the link up for as long as this connection
            // object lives -- without it Windows is free to drop it between operations -- and it
            // is also the only place the negotiated MTU is exposed. Informational enough to
            // survive not being available: the GATT object references below hold the link on
            // their own.
            try
            {
                out.session = awaitOp(GattSession::FromDeviceIdAsync(out.device.BluetoothDeviceId()),
                                      timeout,
                                      "opening a GATT session with " + id);
                if (out.session != nullptr)
                    out.session.MaintainConnection(true);
            }
            catch (const hresult_error&)
            {
                out.session = nullptr;
            }

            // Requesting services is what actually establishes the link on Windows.
            const auto servicesResult =
                awaitOp(out.device.GetGattServicesAsync(BluetoothCacheMode::Uncached),
                        timeout,
                        "reading the GATT database of " + id);

            if (servicesResult.Status() != GattCommunicationStatus::Success)
                throw BleError("Cannot read the GATT database of " + id + " (GATT status " +
                               std::to_string(static_cast<int>(servicesResult.Status())) +
                               "). The peripheral may require pairing.");

            int serviceIndex = 0;
            for (const auto& gattService : servicesResult.Services())
            {
                Service service;
                service.uuid = uuidToString(gattService.Uuid());
                service.primary = true;

                const auto characteristicsResult =
                    awaitOp(gattService.GetCharacteristicsAsync(BluetoothCacheMode::Uncached),
                            timeout,
                            "reading the characteristics of service " + service.uuid);

                if (characteristicsResult.Status() == GattCommunicationStatus::Success)
                {
                    int characteristicIndex = 0;
                    for (const auto& gattCharacteristic : characteristicsResult.Characteristics())
                    {
                        Characteristic characteristic;
                        characteristic.uuid = uuidToString(gattCharacteristic.Uuid());
                        characteristic.properties =
                            mapProperties(gattCharacteristic.CharacteristicProperties());

                        // WinRT has no stable opaque handle to hand back, so synthesise one that
                        // is unique within this connection and keep the object alongside it.
                        characteristic.handle = std::to_string(serviceIndex) + "/" +
                                                std::to_string(characteristicIndex) + "/" +
                                                characteristic.uuid;

                        out.characteristicsByHandle.emplace(characteristic.handle,
                                                            gattCharacteristic);
                        service.characteristics.push_back(std::move(characteristic));
                        ++characteristicIndex;
                    }
                }

                out.services.push_back(std::move(service));
                out.gattServices.push_back(gattService);
                ++serviceIndex;
            }

            // Read once the link is up: before that Windows reports the 23-byte default.
            if (out.session != nullptr)
                out.mtu = out.session.MaxPduSize();

            return out;
        });

        return std::make_unique<WinRtConnection>(apartment,
                                                 std::move(resolved.device),
                                                 std::move(resolved.session),
                                                 std::move(resolved.gattServices),
                                                 std::move(resolved.services),
                                                 std::move(resolved.characteristicsByHandle),
                                                 resolved.mtu);
    }

private:
    // Declared first so it outlives the watcher, which is released through it.
    std::shared_ptr<Apartment> apartment;

    /*!
     * Peripherals Windows currently holds an LE link to, keyed by address.
     *
     * Cached for ConnectedQueryInterval: the enumeration is a device-enumeration query plus one
     * BluetoothLEDevice per result, and getDiscovered() is polled far faster than the set changes.
     * Any failure yields an empty map rather than throwing -- discovery reporting the peripherals
     * it did find beats discovery failing because an enumeration hiccupped.
     */
    std::map<std::string, Advertisement> connectedPeripherals()
    {
        std::scoped_lock guard(connectedSync);

        const auto now = steady_clock::now();
        if (connectedQueriedAt != steady_clock::time_point{} &&
            now - connectedQueriedAt < ConnectedQueryInterval)
        {
            return connectedCache;
        }

        connectedQueriedAt = now;
        connectedCache.clear();

        try
        {
            connectedCache = apartment->run([]() {
                std::map<std::string, Advertisement> found;

                const auto selector = BluetoothLEDevice::GetDeviceSelectorFromConnectionStatus(
                    BluetoothConnectionStatus::Connected);

                const auto infos = awaitOp(DeviceInformation::FindAllAsync(selector),
                                           ConnectedQueryTimeout,
                                           "listing connected Bluetooth devices");

                for (const auto& info : infos)
                {
                    const auto device = awaitOp(BluetoothLEDevice::FromIdAsync(info.Id()),
                                                ConnectedQueryTimeout,
                                                "opening a connected Bluetooth device");

                    if (device == nullptr ||
                        device.ConnectionStatus() != BluetoothConnectionStatus::Connected)
                    {
                        continue;
                    }

                    Advertisement advertisement;
                    advertisement.id = formatAddress(device.BluetoothAddress());
                    advertisement.name = toString(device.Name());
                    advertisement.connected = true;

                    // No reading to report: the peripheral is not advertising. Advertisement
                    // documents rssi 0 as "unknown" for exactly this case.
                    advertisement.rssi = 0;
                    advertisement.lastSeen = steady_clock::now();

                    // Cached, not Uncached: the link is already up, so this reads the services
                    // Windows resolved when it was established rather than going back on air.
                    const auto services =
                        awaitOp(device.GetGattServicesAsync(BluetoothCacheMode::Cached),
                                ConnectedQueryTimeout,
                                "reading the services of a connected Bluetooth device");

                    if (services.Status() == GattCommunicationStatus::Success)
                    {
                        for (const auto& service : services.Services())
                            advertisement.serviceUuids.push_back(uuidToString(service.Uuid()));
                    }

                    found[advertisement.id] = std::move(advertisement);
                }

                return found;
            });
        }
        catch (const std::exception&)
        {
            // BleError, including the translated hresult_error from Apartment::run().
            connectedCache.clear();
        }

        return connectedCache;
    }

    // Everything the watcher delegate touches. Shared rather than owned -- see ScanState.
    std::shared_ptr<ScanState> scanState;

    // Guards the connected-peripheral cache. Separate from scanSync and ScanState::sync: it is
    // taken across a blocking apartment call, which neither of those may be.
    std::mutex connectedSync;
    std::map<std::string, Advertisement> connectedCache;
    steady_clock::time_point connectedQueriedAt{};

    // Guards the watcher and the scanning flag, which no event handler touches. Deliberately
    // separate from ScanState::sync: stopScan() revokes an event while holding this one.
    std::mutex scanSync;
    BluetoothLEAdvertisementWatcher watcher{nullptr};
    event_token receivedToken{};
    std::atomic<bool> scanning{false};
};

}

std::unique_ptr<ICentral> createPlatformCentral()
{
    return std::make_unique<WinRtCentral>();
}

}

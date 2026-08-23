// BLE backend for Linux, speaking to BlueZ over the system D-Bus.
//
// BlueZ exposes adapters, peripherals and their GATT databases as D-Bus objects under
// org.bluez. Discovery is a matter of asking an adapter to scan and then watching the object
// tree; a GATT database is just the subtree beneath a device object once services resolve.

#include "bluez_bus.h"

#include <bluetooth_module/ble/ble_central.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>

namespace opendaq_bt::ble
{

namespace
{

using namespace std::chrono;

constexpr char AdapterInterface[] = "org.bluez.Adapter1";
constexpr char DeviceInterface[] = "org.bluez.Device1";
constexpr char GattServiceInterface[] = "org.bluez.GattService1";
constexpr char GattCharacteristicInterface[] = "org.bluez.GattCharacteristic1";
constexpr char PropertiesInterface[] = "org.freedesktop.DBus.Properties";
constexpr char ObjectManagerInterface[] = "org.freedesktop.DBus.ObjectManager";

const bluez::Value* find(const bluez::Properties& properties, const std::string& key)
{
    const auto it = properties.find(key);
    return it == properties.end() ? nullptr : &it->second;
}

std::string stringProperty(const bluez::Properties& properties, const std::string& key)
{
    const auto* value = find(properties, key);
    return value != nullptr ? value->str : std::string{};
}

bool boolProperty(const bluez::Properties& properties, const std::string& key)
{
    const auto* value = find(properties, key);
    return value != nullptr && value->boolean;
}

/*!
 * BlueZ synthesises an Alias for peripherals that advertise no name, derived from the address
 * ("53-FA-A3-1B-F4-1F" for 53:FA:A3:1B:F4:1F). Presenting that as a device name is worse than
 * presenting none, so recognise it by comparing only the hex digits.
 */
bool isAddressDerivedAlias(const std::string& name, const std::string& address)
{
    const auto significant = [](const std::string& value) {
        std::string result;
        for (const char c : value)
        {
            if (std::isalnum(static_cast<unsigned char>(c)))
                result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
        return result;
    };

    const auto addressDigits = significant(address);
    const auto nameDigits = significant(name);

    return nameDigits == addressDigits || nameDigits == "DEV" + addressDigits;
}

/*! Finds the first adapter object in a GetManagedObjects reply. */
std::string findAdapterPath(const bluez::ManagedObjects& objects, bool& poweredOut)
{
    for (const auto& [path, interfaces] : objects)
    {
        const auto it = interfaces.find(AdapterInterface);
        if (it == interfaces.end())
            continue;

        poweredOut = boolProperty(it->second, "Powered");
        return path;
    }

    poweredOut = false;
    return {};
}

class BluezConnection final : public IConnection
{
public:
    BluezConnection(std::string devicePath, std::vector<Service> services, uint16_t mtuSize)
        : devicePath(std::move(devicePath))
        , resolvedServices(std::move(services))
        , mtuSize(mtuSize)
    {
    }

    ~BluezConnection() override
    {
        // Drop the callbacks before anything else so a notification arriving on the dispatch
        // thread cannot re-enter a half-destroyed object.
        {
            std::scoped_lock guard(callbackMutex);
            disconnectCallback = nullptr;
            notificationCallbacks.clear();
        }

        std::string error;
        for (const auto& handle : subscribedHandles)
            bus.callVoid(handle, GattCharacteristicInterface, "StopNotify", error, 2ull * 1000 * 1000);

        bus.callVoid(devicePath, DeviceInterface, "Disconnect", error, 5ull * 1000 * 1000);
        bus.close();
    }

    bool open(std::string& error)
    {
        if (!bus.open(error))
            return false;

        // Watching Connected lets an unsolicited drop surface as a device property change
        // rather than as a read failure much later.
        bus.matchSignal(devicePath, PropertiesInterface, "PropertiesChanged", &onPropertiesChanged, this, error);
        bus.startDispatch();
        connected = true;
        return true;
    }

    bool isConnected() const override { return connected; }

    const std::vector<Service>& services() const override { return resolvedServices; }

    uint16_t mtu() const override { return mtuSize; }

    std::vector<uint8_t> read(const std::string& handle) override
    {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        sd_bus_message* request = nullptr;

        int r;
        {
            std::scoped_lock guard(bus.lock());
            if (!bus.isOpen())
                throw BleError("Device is disconnected.");

            r = sd_bus_message_new_method_call(bus.raw(),
                                               &request,
                                               "org.bluez",
                                               handle.c_str(),
                                               GattCharacteristicInterface,
                                               "ReadValue");
            if (r >= 0)
                r = sd_bus_message_append(request, "a{sv}", 0);
            if (r >= 0)
                r = sd_bus_call(bus.raw(), request, 10ull * 1000 * 1000, &err, &reply);

            sd_bus_message_unref(request);
        }

        if (r < 0)
        {
            const std::string message = err.message != nullptr ? err.message : "read failed";
            sd_bus_error_free(&err);
            sd_bus_message_unref(reply);
            throw BleError(message);
        }

        std::vector<uint8_t> data;
        const void* raw = nullptr;
        size_t size = 0;
        if (sd_bus_message_read_array(reply, SD_BUS_TYPE_BYTE, &raw, &size) >= 0 && raw != nullptr)
        {
            const auto* first = static_cast<const uint8_t*>(raw);
            data.assign(first, first + size);
        }

        sd_bus_error_free(&err);
        sd_bus_message_unref(reply);

        return data;
    }

    void write(const std::string& handle, const std::vector<uint8_t>& data, bool withResponse) override
    {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        sd_bus_message* request = nullptr;

        int r;
        {
            std::scoped_lock guard(bus.lock());
            if (!bus.isOpen())
                throw BleError("Device is disconnected.");

            r = sd_bus_message_new_method_call(bus.raw(),
                                               &request,
                                               "org.bluez",
                                               handle.c_str(),
                                               GattCharacteristicInterface,
                                               "WriteValue");
            if (r >= 0)
                r = sd_bus_message_append_array(request, SD_BUS_TYPE_BYTE, data.data(), data.size());
            if (r >= 0)
                r = sd_bus_message_append(request, "a{sv}", 1, "type", "s",
                                          withResponse ? "request" : "command");
            if (r >= 0)
                r = sd_bus_call(bus.raw(), request, 10ull * 1000 * 1000, &err, &reply);

            sd_bus_message_unref(request);
        }

        const bool failed = r < 0;
        const std::string message = failed && err.message != nullptr ? err.message : "write failed";

        sd_bus_error_free(&err);
        sd_bus_message_unref(reply);

        if (failed)
            throw BleError(message);
    }

    void subscribe(const std::string& handle, NotificationCallback callback) override
    {
        {
            std::scoped_lock guard(callbackMutex);
            notificationCallbacks[handle] = std::move(callback);
        }

        if (subscribedHandles.count(handle) != 0)
            return;

        std::string error;

        // sd_bus_match_signal matches object paths exactly, and a characteristic's path is known
        // here, so one exact match per subscription is enough -- no path-namespace matching, and
        // no filtering of unrelated org.bluez traffic on the dispatch thread.
        if (!bus.matchSignal(handle, PropertiesInterface, "PropertiesChanged", &onCharacteristicChanged, this, error))
        {
            std::scoped_lock guard(callbackMutex);
            notificationCallbacks.erase(handle);
            throw BleError("Cannot watch characteristic " + handle + ": " + error);
        }

        if (!bus.callVoid(handle, GattCharacteristicInterface, "StartNotify", error))
        {
            std::scoped_lock guard(callbackMutex);
            notificationCallbacks.erase(handle);
            throw BleError("Cannot subscribe to characteristic " + handle + ": " + error);
        }

        subscribedHandles.insert(handle);
    }

    void unsubscribe(const std::string& handle) override
    {
        {
            std::scoped_lock guard(callbackMutex);
            notificationCallbacks.erase(handle);
        }

        if (subscribedHandles.erase(handle) == 0)
            return;

        // The match slot stays installed until the connection closes; with no callback registered
        // its handler is a no-op, and StopNotify is what actually stops the peripheral sending.
        std::string error;
        bus.callVoid(handle, GattCharacteristicInterface, "StopNotify", error, 5ull * 1000 * 1000);
    }

    void setDisconnectCallback(std::function<void()> callback) override
    {
        std::scoped_lock guard(callbackMutex);
        disconnectCallback = std::move(callback);
    }

private:
    /*!
     * BlueZ delivers a notification as a PropertiesChanged on the characteristic object with the
     * new payload under the "Value" key.
     */
    static int onCharacteristicChanged(sd_bus_message* m, void* userdata, sd_bus_error*)
    {
        auto* self = static_cast<BluezConnection*>(userdata);

        const char* interfaceName = nullptr;
        if (sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &interfaceName) < 0 ||
            interfaceName == nullptr || std::string(interfaceName) != GattCharacteristicInterface)
            return 0;

        bluez::Properties changed;
        bluez::readProperties(m, changed);

        const auto valueIt = changed.find("Value");
        if (valueIt == changed.end())
            return 0;

        const char* path = sd_bus_message_get_path(m);
        if (path == nullptr)
            return 0;

        NotificationCallback callback;
        {
            std::scoped_lock guard(self->callbackMutex);
            const auto it = self->notificationCallbacks.find(path);
            if (it == self->notificationCallbacks.end())
                return 0;
            callback = it->second;
        }

        // Invoked outside the lock: the consumer builds openDAQ packets here, and holding the
        // callback mutex across that would serialise it against unsubscribe and teardown.
        if (callback)
            callback(valueIt->second.bytes);

        return 0;
    }

    static int onPropertiesChanged(sd_bus_message* m, void* userdata, sd_bus_error*)
    {
        auto* self = static_cast<BluezConnection*>(userdata);

        const char* interfaceName = nullptr;
        if (sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &interfaceName) < 0 ||
            interfaceName == nullptr || std::string(interfaceName) != DeviceInterface)
            return 0;

        bluez::Properties changed;
        bluez::readProperties(m, changed);

        const auto it = changed.find("Connected");
        if (it == changed.end() || it->second.boolean)
            return 0;

        self->connected = false;

        std::function<void()> callback;
        {
            std::scoped_lock guard(self->callbackMutex);
            callback = self->disconnectCallback;
        }

        if (callback)
            callback();

        return 0;
    }

    bluez::Bus bus;
    const std::string devicePath;
    const std::vector<Service> resolvedServices;
    const uint16_t mtuSize;

    volatile bool connected = false;

    std::mutex callbackMutex;
    std::function<void()> disconnectCallback;
    std::map<std::string, NotificationCallback> notificationCallbacks;

    /*! Touched only from the subscribing thread, never from the dispatch thread. */
    std::set<std::string> subscribedHandles;
};

class BluezCentral final : public ICentral
{
public:
    ~BluezCentral() override
    {
        stopScan();
        bus.close();
    }

    bool isAvailable(std::string& reason) override
    {
        if (!ensureBus(reason))
            return false;

        bluez::ManagedObjects objects;
        if (!bus.getManagedObjects(objects, reason))
        {
            reason = "cannot query BlueZ (" + reason +
                     "). Is the bluetooth service running, and does this user have D-Bus access to org.bluez?";
            return false;
        }

        bool powered = false;
        const auto path = findAdapterPath(objects, powered);

        if (path.empty())
        {
            reason = "no Bluetooth adapter was found";
            return false;
        }

        if (!powered)
        {
            reason = "the Bluetooth adapter (" + path + ") is powered off";
            return false;
        }

        {
            std::scoped_lock guard(stateMutex);
            adapterPath = path;
        }

        return true;
    }

    void startScan() override
    {
        std::string error;
        if (!isAvailable(error))
            throw BleError("Cannot start a Bluetooth scan: " + error);

        std::scoped_lock guard(stateMutex);
        if (scanning)
            return;

        installMatches();

        // Restricting the transport to LE keeps Classic inquiry off the air, and DuplicateData
        // makes BlueZ re-emit PropertiesChanged on every advertisement rather than only when a
        // value changes -- which is what keeps the "last seen" timestamps meaningful.
        if (!setDiscoveryFilter(error))
            throw BleError("Cannot configure the Bluetooth scan filter: " + error);

        if (!bus.callVoid(adapterPath, AdapterInterface, "StartDiscovery", error))
        {
            // BlueZ answers InProgress when something else already has discovery running; that
            // is perfectly usable for us, since results arrive through the object tree either way.
            if (error.find("InProgress") == std::string::npos)
                throw BleError("Cannot start a Bluetooth scan: " + error);
        }

        bus.startDispatch();
        scanning = true;
    }

    void stopScan() override
    {
        std::scoped_lock guard(stateMutex);
        if (!scanning)
            return;

        std::string error;
        bus.callVoid(adapterPath, AdapterInterface, "StopDiscovery", error, 5ull * 1000 * 1000);
        scanning = false;
    }

    bool isScanning() const override { return scanning; }

    std::vector<Advertisement> getDiscovered() override
    {
        bluez::ManagedObjects objects;
        std::string error;
        if (!bus.getManagedObjects(objects, error))
            return {};


        std::vector<Advertisement> result;
        std::scoped_lock guard(stateMutex);

        for (const auto& [path, interfaces] : objects)
        {
            const auto it = interfaces.find(DeviceInterface);
            if (it == interfaces.end())
                continue;

            const auto& properties = it->second;

            const auto address = stringProperty(properties, "Address");
            if (address.empty())
                continue;

            // Report a peripheral this scan actually heard advertising, or one this host is
            // holding a link to.
            //
            // BlueZ's object tree is a cache that outlives any one scan: it holds on to every
            // device seen in recent sessions, RSSI included, and the addresses of peripherals
            // using resolvable private addresses change every few minutes. Trusting the tree
            // therefore reports scores of devices that are nowhere near the adapter. The
            // InterfacesAdded and PropertiesChanged signals, by contrast, fire when an
            // advertisement is genuinely received, and the matches are installed before discovery
            // starts, so nothing in range is missed.
            const auto seenIt = lastSeenByPath.find(path);
            const bool heard = seenIt != lastSeenByPath.end();

            // A connected peripheral stops advertising and BlueZ drops its RSSI property, so it
            // can never be heard -- and it is the one peripheral we are certain we can reach.
            // Being connected is current evidence, not cache, so the reasoning above does not
            // apply to it. ServicesResolved is required as well because this loop walks the whole
            // object tree, which is not restricted to LE the way the scan filter is: Connected
            // alone would start reporting Bluetooth Classic headsets, which have no GATT database
            // and never resolve one.
            const bool linked = boolProperty(properties, "Connected") &&
                                boolProperty(properties, "ServicesResolved");

            if (!heard && !linked)
                continue;

            const auto* rssi = find(properties, "RSSI");

            Advertisement advertisement;
            advertisement.id = address;
            advertisement.rssi = rssi != nullptr ? static_cast<int16_t>(rssi->integer) : 0;
            advertisement.connected = linked;

            // A peripheral that stopped advertising when it connected has a sighting that only
            // recedes; a default-constructed one it never had at all is worse. Either way the
            // link itself is the evidence, so stamp the moment we observed it.
            advertisement.lastSeen = heard ? seenIt->second : steady_clock::now();

            advertisement.name = stringProperty(properties, "Name");
            if (advertisement.name.empty())
                advertisement.name = stringProperty(properties, "Alias");
            if (isAddressDerivedAlias(advertisement.name, address))
                advertisement.name.clear();

            if (const auto* uuids = find(properties, "UUIDs"))
                advertisement.serviceUuids = uuids->strings;

            if (const auto* manufacturer = find(properties, "ManufacturerData"))
                advertisement.manufacturerData = manufacturer->byteArraysByKey;

            result.push_back(std::move(advertisement));
        }

        return result;
    }

    std::unique_ptr<IConnection> connect(const std::string& id, milliseconds timeout) override
    {
        std::string error;
        if (!isAvailable(error))
            throw BleError("Cannot connect: " + error);

        std::string devicePath;
        {
            std::scoped_lock guard(stateMutex);
            devicePath = adapterPath + "/" + bluez::addressToNodeName(id);
        }

        bluez::ManagedObjects objects;
        if (!bus.getManagedObjects(objects, error))
            throw BleError("Cannot query BlueZ: " + error);

        const auto objectIt = objects.find(devicePath);
        if (objectIt == objects.end())
            throw BleError("Bluetooth device " + id +
                           " is not known to BlueZ. Run a discovery scan first so the adapter can find it.");

        // A link left behind by an earlier session -- or held by another application -- is usable
        // as it stands: BlueZ shares it, and the GATT subtree is already in the object tree. Not
        // asking for a connection this host already has also sidesteps the question of what
        // Device1.Connect does in that case, which is not the same answer across BlueZ versions.
        bool alreadyConnected = false;
        if (const auto interfaceIt = objectIt->second.find(DeviceInterface);
            interfaceIt != objectIt->second.end())
        {
            alreadyConnected = boolProperty(interfaceIt->second, "Connected");
        }

        if (!alreadyConnected && !bus.callVoid(devicePath,
                                               DeviceInterface,
                                               "Connect",
                                               error,
                                               static_cast<uint64_t>(timeout.count()) * 1000))
        {
            // As with InProgress in startScan(): the link coming up underneath us is success.
            if (error.find("AlreadyConnected") == std::string::npos)
                throw BleError("Cannot connect to " + id + ": " + error);
        }

        // The timeout is a per-phase budget rather than one shared across both. Establishing the
        // link and resolving the GATT database are independent waits, and on a busy adapter a slow
        // connect would otherwise leave service discovery almost no time and fail a connection
        // that was about to succeed.
        waitForServicesResolved(devicePath, steady_clock::now() + timeout);

        uint16_t mtuSize = 0;
        auto services = collectServices(devicePath, mtuSize);

        auto connection = std::make_unique<BluezConnection>(devicePath, std::move(services), mtuSize);
        if (!connection->open(error))
            throw BleError("Connected to " + id + " but cannot watch it: " + error);

        return connection;
    }

private:
    bool ensureBus(std::string& reason)
    {
        std::scoped_lock guard(stateMutex);
        return bus.open(reason);
    }

    void installMatches()
    {
        if (matchesInstalled)
            return;

        std::string error;
        bus.matchSignal("/", ObjectManagerInterface, "InterfacesAdded", &onInterfacesAdded, this, error);
        bus.matchSignal("", PropertiesInterface, "PropertiesChanged", &onDevicePropertiesChanged, this, error);
        bus.matchSignal("/", ObjectManagerInterface, "InterfacesRemoved", &onInterfacesRemoved, this, error);

        matchesInstalled = true;
    }

    bool setDiscoveryFilter(std::string& error)
    {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message* request = nullptr;
        sd_bus_message* reply = nullptr;

        std::scoped_lock guard(bus.lock());
        if (!bus.isOpen())
        {
            error = "bus is not connected";
            return false;
        }

        int r = sd_bus_message_new_method_call(
            bus.raw(), &request, "org.bluez", adapterPath.c_str(), AdapterInterface, "SetDiscoveryFilter");

        if (r >= 0)
            r = sd_bus_message_open_container(request, SD_BUS_TYPE_ARRAY, "{sv}");
        if (r >= 0)
            r = sd_bus_message_append(request, "{sv}", "Transport", "s", "le");
        if (r >= 0)
            r = sd_bus_message_append(request, "{sv}", "DuplicateData", "b", 1);
        if (r >= 0)
            r = sd_bus_message_close_container(request);
        if (r >= 0)
            r = sd_bus_call(bus.raw(), request, 10ull * 1000 * 1000, &err, &reply);

        if (r < 0 && err.message != nullptr)
            error = err.message;

        sd_bus_message_unref(request);
        sd_bus_message_unref(reply);
        sd_bus_error_free(&err);

        return r >= 0;
    }

    void waitForServicesResolved(const std::string& devicePath, steady_clock::time_point deadline)
    {
        std::string error;

        while (steady_clock::now() < deadline)
        {
            bluez::Value value;
            if (bus.getProperty(devicePath, DeviceInterface, "ServicesResolved", value, error) &&
                value.boolean)
                return;

            std::this_thread::sleep_for(milliseconds(100));
        }

        throw BleError("Timed out waiting for the GATT database of " + devicePath +
                       " to resolve. The peripheral may require pairing.");
    }

    std::vector<Service> collectServices(const std::string& devicePath, uint16_t& mtuOut)
    {
        bluez::ManagedObjects objects;
        std::string error;
        if (!bus.getManagedObjects(objects, error))
            throw BleError("Cannot read the GATT database: " + error);

        // Services and characteristics are nested beneath the device object, and each
        // characteristic names its owning service, so one pass over each is enough.
        std::map<std::string, Service> servicesByPath;

        for (const auto& [path, interfaces] : objects)
        {
            const auto it = interfaces.find(GattServiceInterface);
            if (it == interfaces.end() || path.rfind(devicePath + "/", 0) != 0)
                continue;

            Service service;
            service.uuid = stringProperty(it->second, "UUID");
            service.primary = boolProperty(it->second, "Primary");
            servicesByPath[path] = std::move(service);
        }

        for (const auto& [path, interfaces] : objects)
        {
            const auto it = interfaces.find(GattCharacteristicInterface);
            if (it == interfaces.end())
                continue;

            const auto servicePath = stringProperty(it->second, "Service");
            const auto serviceIt = servicesByPath.find(servicePath);
            if (serviceIt == servicesByPath.end())
                continue;

            Characteristic characteristic;
            characteristic.uuid = stringProperty(it->second, "UUID");
            characteristic.handle = path;

            if (const auto* flags = find(it->second, "Flags"))
                characteristic.properties = bluez::flagsToProperties(flags->strings);

            if (const auto* mtu = find(it->second, "MTU"))
                mtuOut = std::max(mtuOut, static_cast<uint16_t>(mtu->integer));

            serviceIt->second.characteristics.push_back(std::move(characteristic));
        }

        std::vector<Service> services;
        services.reserve(servicesByPath.size());
        for (auto& [path, service] : servicesByPath)
            services.push_back(std::move(service));

        return services;
    }

    void touch(const std::string& path)
    {
        std::scoped_lock guard(stateMutex);
        lastSeenByPath[path] = steady_clock::now();
    }

    /*!
     * Deliberately does not mark the device as seen.
     *
     * BlueZ re-announces every peripheral in its persistent cache through InterfacesAdded the
     * moment discovery starts, complete with the RSSI from whenever it was last observed. Treating
     * that as a sighting reports every device the adapter has met in recent days -- well over a
     * hundred of them in a dense radio environment. Only a subsequent RSSI update proves that a
     * peripheral is advertising right now.
     */
    static int onInterfacesAdded(sd_bus_message*, void*, sd_bus_error*)
    {
        return 0;
    }

    static int onInterfacesRemoved(sd_bus_message* m, void* userdata, sd_bus_error*)
    {
        const char* path = nullptr;
        if (sd_bus_message_read_basic(m, SD_BUS_TYPE_OBJECT_PATH, &path) < 0 || path == nullptr)
            return 0;

        auto* self = static_cast<BluezCentral*>(userdata);
        std::scoped_lock guard(self->stateMutex);
        self->lastSeenByPath.erase(path);

        return 0;
    }

    static int onDevicePropertiesChanged(sd_bus_message* m, void* userdata, sd_bus_error*)
    {
        const char* interfaceName = nullptr;
        if (sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &interfaceName) < 0 ||
            interfaceName == nullptr || std::string(interfaceName) != DeviceInterface)
            return 0;

        bluez::Properties changed;
        bluez::readProperties(m, changed);

        // An RSSI update means the adapter just received an advertisement from this peripheral --
        // the one event that reliably distinguishes a device in range from a cached one. The
        // discovery filter sets DuplicateData, so BlueZ emits these for every advertisement
        // rather than only when the value happens to change.
        if (changed.find("RSSI") == changed.end())
            return 0;

        const char* path = sd_bus_message_get_path(m);
        if (path != nullptr)
            static_cast<BluezCentral*>(userdata)->touch(path);

        return 0;
    }

    bluez::Bus bus;

    mutable std::recursive_mutex stateMutex;
    std::string adapterPath;
    std::map<std::string, steady_clock::time_point> lastSeenByPath;
    bool matchesInstalled = false;
    volatile bool scanning = false;
};

}

std::unique_ptr<ICentral> createPlatformCentral()
{
    return std::make_unique<BluezCentral>();
}

}

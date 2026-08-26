// Android BLE backend. A Java shim exposes android.bluetooth through JNI.

#include "android_jni.h"

#include <bluetooth_module/ble/ble_central.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace opendaq_bt::ble
{

using namespace std::chrono;

namespace
{

constexpr int GattSuccess = 0;

constexpr int StateConnected = 2;

constexpr int RequestedMtu = 517;

constexpr milliseconds MtuTimeout{2000};

// --- State blocks -----------------------------------------------------------------------------

// Android permits one outstanding operation per BluetoothGatt.
enum class Operation
{
    None,
    Read,
    Write,
    DescriptorWrite,
    Mtu
};

struct ConnectionState
{
    std::mutex mutex;
    std::condition_variable changed;

    jobject bridge = nullptr;

    bool connected = false;
    bool connectionSettled = false;
    int connectionStatus = GattSuccess;

    bool servicesResolved = false;
    int servicesStatus = GattSuccess;

    uint16_t mtu = 0;

    Operation pendingOperation = Operation::None;
    std::string pendingHandle;
    std::vector<uint8_t> pendingValue;
    int pendingStatus = GattSuccess;
    bool pendingComplete = false;

    std::map<std::string, NotificationCallback> subscriptions;
    std::function<void()> disconnectCallback;
};

struct ScanState
{
    std::mutex mutex;

    jobject bridge = nullptr;

    std::map<std::string, Advertisement> discovered;
};

// --- Token registries -------------------------------------------------------------------------
//
// Tokens and weak references make late platform callbacks harmless.

std::mutex registryMutex;
std::map<int64_t, std::weak_ptr<ScanState>> scanRegistry;
std::map<int64_t, std::weak_ptr<ConnectionState>> connectionRegistry;
std::atomic<int64_t> nextToken{1};

std::shared_ptr<ScanState> scanStateFor(int64_t token)
{
    std::scoped_lock lock(registryMutex);
    const auto it = scanRegistry.find(token);
    return it == scanRegistry.end() ? nullptr : it->second.lock();
}

std::shared_ptr<ConnectionState> connectionStateFor(int64_t token)
{
    std::scoped_lock lock(registryMutex);
    const auto it = connectionRegistry.find(token);
    return it == connectionRegistry.end() ? nullptr : it->second.lock();
}

// --- Cached Java API --------------------------------------------------------------------------

// Cached during initialize(), whose class loader can resolve application classes.
struct JavaApi
{
    jclass openDaqBluetooth = nullptr;
    jmethodID unavailableReason = nullptr;

    jclass scanBridge = nullptr;
    jmethodID scanBridgeInit = nullptr;
    jmethodID startScan = nullptr;
    jmethodID stopScan = nullptr;
    jmethodID isScanning = nullptr;
    jmethodID connectedAddresses = nullptr;

    jclass gattBridge = nullptr;
    jmethodID gattConnect = nullptr;
    jmethodID gattClose = nullptr;
    jmethodID discoverServices = nullptr;
    jmethodID requestMtu = nullptr;
    jmethodID describeServices = nullptr;
    jmethodID readCharacteristic = nullptr;
    jmethodID writeCharacteristic = nullptr;
    jmethodID setNotify = nullptr;

    std::atomic<bool> ready{false};
};

JavaApi api;

jclass globalClass(JNIEnv* env, const char* name)
{
    const jclass local = env->FindClass(name);
    if (local == nullptr)
    {
        jni::takeException(env);
        return nullptr;
    }

    const auto global = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    return global;
}

void cacheJavaApi(JNIEnv* env)
{
    if (api.ready.load(std::memory_order_acquire) || env == nullptr)
        return;

    api.openDaqBluetooth = globalClass(env, "com/opendaq/bt/OpenDaqBluetooth");
    api.scanBridge = globalClass(env, "com/opendaq/bt/ScanBridge");
    api.gattBridge = globalClass(env, "com/opendaq/bt/GattBridge");

    if (api.openDaqBluetooth == nullptr || api.scanBridge == nullptr || api.gattBridge == nullptr)
        return;

    api.unavailableReason = env->GetStaticMethodID(
        api.openDaqBluetooth, "unavailableReason",
        "(Landroid/content/Context;)Ljava/lang/String;");

    api.scanBridgeInit =
        env->GetMethodID(api.scanBridge, "<init>", "(Landroid/content/Context;J)V");
    api.startScan = env->GetMethodID(api.scanBridge, "startScan", "([Ljava/lang/String;)Z");
    api.stopScan = env->GetMethodID(api.scanBridge, "stopScan", "()V");
    api.isScanning = env->GetMethodID(api.scanBridge, "isScanning", "()Z");
    api.connectedAddresses = env->GetStaticMethodID(
        api.scanBridge, "connectedAddresses", "(Landroid/content/Context;)[Ljava/lang/String;");

    api.gattConnect = env->GetStaticMethodID(
        api.gattBridge, "connect",
        "(Landroid/content/Context;Ljava/lang/String;J)Lcom/opendaq/bt/GattBridge;");
    api.gattClose = env->GetMethodID(api.gattBridge, "close", "()V");
    api.discoverServices = env->GetMethodID(api.gattBridge, "discoverServices", "()Z");
    api.requestMtu = env->GetMethodID(api.gattBridge, "requestMtu", "(I)Z");
    api.describeServices =
        env->GetMethodID(api.gattBridge, "describeServices", "()[Ljava/lang/String;");
    api.readCharacteristic = env->GetMethodID(api.gattBridge, "read", "(Ljava/lang/String;)Z");
    api.writeCharacteristic =
        env->GetMethodID(api.gattBridge, "write", "(Ljava/lang/String;[BZ)Z");
    api.setNotify = env->GetMethodID(api.gattBridge, "setNotify", "(Ljava/lang/String;ZZ)Z");

    jni::takeException(env);

    api.ready.store(api.unavailableReason != nullptr && api.scanBridgeInit != nullptr
             && api.startScan != nullptr && api.stopScan != nullptr && api.isScanning != nullptr
             && api.connectedAddresses != nullptr && api.gattConnect != nullptr
             && api.gattClose != nullptr && api.discoverServices != nullptr
             && api.requestMtu != nullptr && api.describeServices != nullptr
             && api.readCharacteristic != nullptr && api.writeCharacteristic != nullptr
             && api.setNotify != nullptr,
                   std::memory_order_release);
}

std::vector<std::string> toStringVector(JNIEnv* env, jobjectArray array)
{
    if (array == nullptr)
        return {};

    const auto count = env->GetArrayLength(array);
    std::vector<std::string> result;
    result.reserve(static_cast<size_t>(count));

    for (jsize i = 0; i < count; ++i)
    {
        const auto element = static_cast<jstring>(env->GetObjectArrayElement(array, i));
        result.push_back(jni::toStdString(env, element));
        env->DeleteLocalRef(element);
    }

    return result;
}

}

namespace
{

// --- Connection ---------------------------------------------------------------------------------

class AndroidConnection final : public IConnection
{
public:
    AndroidConnection(int64_t token,
                      std::shared_ptr<ConnectionState> state,
                      std::vector<Service> services)
        : token(token)
        , state(std::move(state))
        , resolvedServices(std::move(services))
    {
    }

    ~AndroidConnection() override
    {
        // Disable callbacks before removing the registry entry.
        {
            std::scoped_lock lock(state->mutex);
            state->subscriptions.clear();
            state->disconnectCallback = nullptr;
        }

        {
            std::scoped_lock lock(registryMutex);
            connectionRegistry.erase(token);
        }

        jni::ScopedEnv env;
        if (!env)
            return;

        std::scoped_lock lock(state->mutex);
        if (state->bridge == nullptr)
            return;

        env->CallVoidMethod(state->bridge, api.gattClose);
        jni::takeException(env.get());

        env->DeleteGlobalRef(state->bridge);
        state->bridge = nullptr;
    }

    bool isConnected() const override
    {
        std::scoped_lock lock(state->mutex);
        return state->connected;
    }

    const std::vector<Service>& services() const override
    {
        return resolvedServices;
    }

    uint16_t mtu() const override
    {
        std::scoped_lock lock(state->mutex);
        return state->mtu;
    }

    std::vector<uint8_t> read(const std::string& handle) override
    {
        std::scoped_lock serialise(operationMutex);

        jni::ScopedEnv env;
        requireEnv(env);

        beginOperation(Operation::Read, handle);

        const jstring javaHandle = jni::toJavaString(env.get(), handle);
        const auto accepted =
            env->CallBooleanMethod(state->bridge, api.readCharacteristic, javaHandle) == JNI_TRUE;
        env->DeleteLocalRef(javaHandle);
        jni::throwIfException(env.get(), "Reading " + handleDescription(handle));

        if (!accepted)
        {
            cancelOperation();
            throw BleError("The Android Bluetooth stack refused to read " + handleDescription(handle) + ".");
        }

        return awaitOperation("read", handle);
    }

    void write(const std::string& handle,
               const std::vector<uint8_t>& data,
               bool withResponse) override
    {
        std::scoped_lock serialise(operationMutex);

        jni::ScopedEnv env;
        requireEnv(env);

        beginOperation(Operation::Write, handle);

        const jstring javaHandle = jni::toJavaString(env.get(), handle);
        const jbyteArray javaValue = jni::toJavaBytes(env.get(), data);

        const auto accepted = env->CallBooleanMethod(state->bridge, api.writeCharacteristic,
                                                     javaHandle, javaValue,
                                                     withResponse ? JNI_TRUE : JNI_FALSE) == JNI_TRUE;

        env->DeleteLocalRef(javaValue);
        env->DeleteLocalRef(javaHandle);
        jni::throwIfException(env.get(), "Writing " + handleDescription(handle));

        if (!accepted)
        {
            cancelOperation();
            throw BleError("The Android Bluetooth stack refused to write " + handleDescription(handle) + ".");
        }

        awaitOperation("write", handle);
    }

    void subscribe(const std::string& handle, NotificationCallback callback) override
    {
        const auto* characteristic = findCharacteristic(handle);
        if (characteristic == nullptr)
            throw BleError("Characteristic " + handle + " is not part of this peripheral.");

        const auto notifies = (characteristic->properties & CharPropNotify) != 0;
        const auto indicates = (characteristic->properties & CharPropIndicate) != 0;

        if (!notifies && !indicates)
        {
            throw BleError("Characteristic " + handleDescription(handle)
                           + " neither notifies nor indicates.");
        }

        {
            // Register before the CCCD write to avoid dropping an immediate notification.
            std::scoped_lock lock(state->mutex);
            state->subscriptions[handle] = std::move(callback);
        }

        try
        {
            setNotify(handle, true, !notifies && indicates);
        }
        catch (...)
        {
            std::scoped_lock lock(state->mutex);
            state->subscriptions.erase(handle);
            throw;
        }
    }

    void unsubscribe(const std::string& handle) override
    {
        {
            std::scoped_lock lock(state->mutex);
            if (state->subscriptions.erase(handle) == 0)
                return;
        }

        setNotify(handle, false, false);
    }

    void setDisconnectCallback(std::function<void()> callback) override
    {
        std::scoped_lock lock(state->mutex);
        state->disconnectCallback = std::move(callback);
    }

private:
    void requireEnv(const jni::ScopedEnv& env) const
    {
        if (!env)
            throw BleError("This thread could not be attached to the Java VM.");

        std::scoped_lock lock(state->mutex);
        if (state->bridge == nullptr)
            throw BleError("The connection to this peripheral has been closed.");
    }

    const Characteristic* findCharacteristic(const std::string& handle) const
    {
        for (const auto& service : resolvedServices)
        {
            for (const auto& characteristic : service.characteristics)
            {
                if (characteristic.handle == handle)
                    return &characteristic;
            }
        }

        return nullptr;
    }

    std::string handleDescription(const std::string& handle) const
    {
        const auto* characteristic = findCharacteristic(handle);
        return characteristic == nullptr ? handle : characteristic->uuid + " (" + handle + ")";
    }

    void setNotify(const std::string& handle, bool enable, bool indicate)
    {
        std::scoped_lock serialise(operationMutex);

        jni::ScopedEnv env;
        requireEnv(env);

        beginOperation(Operation::DescriptorWrite, handle);

        const jstring javaHandle = jni::toJavaString(env.get(), handle);
        const auto accepted = env->CallBooleanMethod(state->bridge, api.setNotify, javaHandle,
                                                     enable ? JNI_TRUE : JNI_FALSE,
                                                     indicate ? JNI_TRUE : JNI_FALSE) == JNI_TRUE;
        env->DeleteLocalRef(javaHandle);
        jni::throwIfException(env.get(), "Subscribing to " + handleDescription(handle));

        if (!accepted)
        {
            cancelOperation();
            throw BleError("The Android Bluetooth stack refused to " + std::string(enable ? "enable" : "disable")
                           + " notifications for " + handleDescription(handle)
                           + ". A characteristic that notifies must also carry a Client "
                             "Characteristic Configuration descriptor.");
        }

        awaitOperation(enable ? "subscribe to" : "unsubscribe from", handle);
    }

    void beginOperation(Operation operation, const std::string& handle)
    {
        std::scoped_lock lock(state->mutex);

        if (!state->connected)
            throw BleError("The peripheral is not connected.");

        state->pendingOperation = operation;
        state->pendingHandle = handle;
        state->pendingComplete = false;
        state->pendingStatus = GattSuccess;
        state->pendingValue.clear();
    }

    void cancelOperation()
    {
        std::scoped_lock lock(state->mutex);
        state->pendingOperation = Operation::None;
        state->pendingComplete = false;
    }

    std::vector<uint8_t> awaitOperation(const char* what, const std::string& handle)
    {
        std::unique_lock lock(state->mutex);

        const auto settled = state->changed.wait_for(lock, operationTimeout, [&] {
            return state->pendingComplete || !state->connected;
        });

        const auto complete = state->pendingComplete;
        const auto status = state->pendingStatus;
        auto value = std::move(state->pendingValue);
        const auto stillConnected = state->connected;

        state->pendingOperation = Operation::None;
        state->pendingComplete = false;
        state->pendingValue.clear();

        lock.unlock();

        if (!settled || !complete)
        {
            if (!stillConnected)
                throw BleError("The peripheral disconnected while trying to " + std::string(what) + " "
                               + handleDescription(handle) + ".");

            throw BleError("Timed out trying to " + std::string(what) + " "
                           + handleDescription(handle) + ".");
        }

        if (status != GattSuccess)
        {
            throw BleError("Failed to " + std::string(what) + " " + handleDescription(handle)
                           + ": the peripheral answered with GATT status " + std::to_string(status)
                           + ". A characteristic that requires encryption needs the peripheral to "
                             "be paired in Android's Bluetooth settings first.");
        }

        return value;
    }

    const int64_t token;
    const std::shared_ptr<ConnectionState> state;
    const std::vector<Service> resolvedServices;

    std::mutex operationMutex;

    static constexpr milliseconds operationTimeout{10000};
};

// --- Central ------------------------------------------------------------------------------------

class AndroidCentral final : public ICentral
{
public:
    AndroidCentral()
        : scanToken(nextToken.fetch_add(1))
        , scan(std::make_shared<ScanState>())
    {
        std::scoped_lock lock(registryMutex);
        scanRegistry[scanToken] = scan;
    }

    ~AndroidCentral() override
    {
        stopScan();

        {
            std::scoped_lock lock(registryMutex);
            scanRegistry.erase(scanToken);
        }

        jni::ScopedEnv env;
        if (!env)
            return;

        std::scoped_lock lock(scan->mutex);
        if (scan->bridge != nullptr)
        {
            env->DeleteGlobalRef(scan->bridge);
            scan->bridge = nullptr;
        }
    }

    bool isAvailable(std::string& reason) override
    {
        if (!jni::isInitialized())
        {
            reason = "OpenDaqBluetooth.initialize(context) has not been called, so this module has "
                     "no Android Context to open the Bluetooth adapter with. Call it, from the "
                     "application, before building the openDAQ Instance";
            return false;
        }

        jni::ScopedEnv env;
        if (!env)
        {
            reason = "this thread could not be attached to the Java VM";
            return false;
        }

        if (!api.ready.load(std::memory_order_acquire))
        {
            reason = "the com.opendaq.bt Java classes could not be found. They ship in the same "
                     "AAR as this module; a release build that shrank them away is the usual cause";
            return false;
        }

        const auto context = jni::applicationContext(env.get());
        const auto text = static_cast<jstring>(env->CallStaticObjectMethod(
            api.openDaqBluetooth, api.unavailableReason, context));
        env->DeleteLocalRef(context);

        if (const auto failure = jni::takeException(env.get()); !failure.empty())
        {
            reason = "the Bluetooth adapter could not be queried: " + failure;
            return false;
        }

        // A null return means nothing is wrong; anything else is the reason, ready to show.
        if (text == nullptr)
            return true;

        reason = jni::toStdString(env.get(), text);
        env->DeleteLocalRef(text);
        return false;
    }

    void startScan() override
    {
        std::string reason;
        if (!isAvailable(reason))
            throw BleError("Bluetooth is unavailable: " + reason + ".");

        jni::ScopedEnv env;
        if (!env)
            throw BleError("This thread could not be attached to the Java VM.");

        const auto bridge = ensureBridge(env.get());

        // Filtering belongs above the backend seam.
        const auto started =
            env->CallBooleanMethod(bridge, api.startScan, static_cast<jobjectArray>(nullptr)) == JNI_TRUE;
        jni::throwIfException(env.get(), "Starting a Bluetooth scan");

        if (!started)
            throw BleError("The Android Bluetooth stack refused to start a scan.");
    }

    void stopScan() override
    {
        jni::ScopedEnv env;
        if (!env || !api.ready.load(std::memory_order_acquire))
            return;

        jobject bridge = nullptr;
        {
            std::scoped_lock lock(scan->mutex);
            bridge = scan->bridge;
        }

        if (bridge == nullptr)
            return;

        env->CallVoidMethod(bridge, api.stopScan);
        jni::takeException(env.get());
    }

    bool isScanning() const override
    {
        jni::ScopedEnv env;
        if (!env || !api.ready.load(std::memory_order_acquire))
            return false;

        jobject bridge = nullptr;
        {
            std::scoped_lock lock(scan->mutex);
            bridge = scan->bridge;
        }

        if (bridge == nullptr)
            return false;

        const auto scanning = env->CallBooleanMethod(bridge, api.isScanning) == JNI_TRUE;
        jni::takeException(env.get());
        return scanning;
    }

    std::vector<Advertisement> getDiscovered() override
    {
        std::vector<Advertisement> result;

        {
            std::scoped_lock lock(scan->mutex);
            result.reserve(scan->discovered.size());
            for (const auto& [address, advertisement] : scan->discovered)
                result.push_back(advertisement);
        }

        appendConnected(result);
        return result;
    }

    std::unique_ptr<IConnection> connect(const std::string& id, milliseconds timeout) override
    {
        std::string reason;
        if (!isAvailable(reason))
            throw BleError("Bluetooth is unavailable: " + reason + ".");

        jni::ScopedEnv env;
        if (!env)
            throw BleError("This thread could not be attached to the Java VM.");

        const auto token = nextToken.fetch_add(1);
        auto state = std::make_shared<ConnectionState>();

        {
            std::scoped_lock lock(registryMutex);
            connectionRegistry[token] = state;
        }

        // Cleans up the registry and scarce GATT client on handshake failure.
        struct Guard
        {
            ~Guard()
            {
                if (!armed)
                    return;

                {
                    std::scoped_lock lock(registryMutex);
                    connectionRegistry.erase(token);
                }

                jni::ScopedEnv env;
                if (!env)
                    return;

                std::scoped_lock lock(state->mutex);
                if (state->bridge == nullptr)
                    return;

                env->CallVoidMethod(state->bridge, api.gattClose);
                jni::takeException(env.get());

                env->DeleteGlobalRef(state->bridge);
                state->bridge = nullptr;
            }

            int64_t token;
            std::shared_ptr<ConnectionState> state;
            bool armed = true;
        } guard{token, state};

        const auto context = jni::applicationContext(env.get());
        const jstring address = jni::toJavaString(env.get(), id);
        const auto bridge = env->CallStaticObjectMethod(api.gattBridge, api.gattConnect,
                                                        context, address, token);
        env->DeleteLocalRef(address);
        env->DeleteLocalRef(context);
        jni::throwIfException(env.get(), "Connecting to " + id);

        if (bridge == nullptr)
        {
            throw BleError("Could not open a GATT connection to " + id
                           + ". Check that it is a Bluetooth address this device recognises.");
        }

        {
            std::scoped_lock lock(state->mutex);
            state->bridge = env->NewGlobalRef(bridge);
        }
        env->DeleteLocalRef(bridge);

        awaitConnected(state, id, timeout);
        auto services = resolveServices(env.get(), state, id, timeout);
        negotiateMtu(env.get(), state);

        guard.armed = false;
        return std::make_unique<AndroidConnection>(token, std::move(state), std::move(services));
    }

private:
    jobject ensureBridge(JNIEnv* env)
    {
        std::scoped_lock lock(scan->mutex);

        if (scan->bridge != nullptr)
            return scan->bridge;

        const auto context = jni::applicationContext(env);
        const auto local = env->NewObject(api.scanBridge, api.scanBridgeInit, context, scanToken);
        env->DeleteLocalRef(context);
        jni::throwIfException(env, "Creating the Bluetooth scanner");

        if (local == nullptr)
            throw BleError("Could not create the Bluetooth scanner.");

        scan->bridge = env->NewGlobalRef(local);
        env->DeleteLocalRef(local);
        return scan->bridge;
    }

    // Include connected peripherals that no longer advertise.
    void appendConnected(std::vector<Advertisement>& result)
    {
        jni::ScopedEnv env;
        if (!env || !api.ready.load(std::memory_order_acquire))
            return;

        const auto context = jni::applicationContext(env.get());
        const auto array = static_cast<jobjectArray>(env->CallStaticObjectMethod(
            api.scanBridge, api.connectedAddresses, context));
        env->DeleteLocalRef(context);

        jni::takeException(env.get());
        if (array == nullptr)
            return;

        const auto addresses = toStringVector(env.get(), array);
        env->DeleteLocalRef(array);

        for (const auto& address : addresses)
        {
            const auto known = std::find_if(result.begin(), result.end(), [&](const Advertisement& a) {
                return a.id == address;
            });

            if (known != result.end())
            {
                known->connected = true;
                continue;
            }

            Advertisement advertisement;
            advertisement.id = address;
            advertisement.connected = true;

            // rssi stays 0: unknown rather than very weak, since nothing measured it.
            advertisement.lastSeen = steady_clock::now();
            result.push_back(std::move(advertisement));
        }
    }

    void awaitConnected(const std::shared_ptr<ConnectionState>& state,
                        const std::string& id,
                        milliseconds timeout)
    {
        std::unique_lock lock(state->mutex);

        const auto settled = state->changed.wait_for(lock, timeout, [&] {
            return state->connectionSettled;
        });

        if (!settled)
            throw BleError("Timed out connecting to " + id + ".");

        if (!state->connected)
        {
            throw BleError("Could not connect to " + id + ": the peripheral answered with GATT "
                           "status " + std::to_string(state->connectionStatus) + ".");
        }
    }

    std::vector<Service> resolveServices(JNIEnv* env,
                                         const std::shared_ptr<ConnectionState>& state,
                                         const std::string& id,
                                         milliseconds timeout)
    {
        jobject bridge = nullptr;
        {
            std::scoped_lock lock(state->mutex);
            bridge = state->bridge;
        }

        if (env->CallBooleanMethod(bridge, api.discoverServices) != JNI_TRUE)
        {
            jni::takeException(env);
            throw BleError("Could not start GATT service discovery on " + id + ".");
        }

        jni::throwIfException(env, "Discovering the services of " + id);

        {
            std::unique_lock lock(state->mutex);
            const auto settled = state->changed.wait_for(lock, timeout, [&] {
                return state->servicesResolved || !state->connected;
            });

            if (!state->connected)
                throw BleError("The peripheral " + id + " disconnected while resolving its GATT database.");

            if (!settled || !state->servicesResolved)
                throw BleError("Timed out resolving the GATT database of " + id + ".");

            if (state->servicesStatus != GattSuccess)
            {
                throw BleError("Could not resolve the GATT database of " + id
                               + ": GATT status " + std::to_string(state->servicesStatus) + ".");
            }
        }

        const auto array =
            static_cast<jobjectArray>(env->CallObjectMethod(bridge, api.describeServices));
        jni::throwIfException(env, "Reading the GATT database of " + id);

        const auto fields = toStringVector(env, array);
        if (array != nullptr)
            env->DeleteLocalRef(array);

        return buildServices(fields);
    }

    // Rebuild the service tree from GattBridge's six-field characteristic rows.
    static std::vector<Service> buildServices(const std::vector<std::string>& fields)
    {
        constexpr size_t fieldsPerCharacteristic = 6;

        std::vector<Service> services;
        std::unordered_map<std::string, size_t> indexByInstance;

        for (size_t i = 0; i + fieldsPerCharacteristic <= fields.size(); i += fieldsPerCharacteristic)
        {
            const auto& handle = fields[i];
            const auto& serviceUuid = fields[i + 1];
            const auto& serviceInstance = fields[i + 2];
            const auto primary = fields[i + 3] == "1";
            const auto& characteristicUuid = fields[i + 4];

            const auto key = serviceUuid + "/" + serviceInstance;

            auto position = indexByInstance.find(key);
            if (position == indexByInstance.end())
            {
                Service service;
                service.uuid = expandUuid(serviceUuid);
                service.primary = primary;

                services.push_back(std::move(service));
                position = indexByInstance.emplace(key, services.size() - 1).first;
            }

            Characteristic characteristic;
            characteristic.uuid = expandUuid(characteristicUuid);
            characteristic.handle = handle;

            characteristic.properties = static_cast<uint32_t>(std::strtoul(
                fields[i + 5].c_str(), nullptr, 10));

            services[position->second].characteristics.push_back(std::move(characteristic));
        }

        return services;
    }

    void negotiateMtu(JNIEnv* env, const std::shared_ptr<ConnectionState>& state)
    {
        jobject bridge = nullptr;
        {
            std::scoped_lock lock(state->mutex);
            bridge = state->bridge;
            state->pendingOperation = Operation::Mtu;
            state->pendingComplete = false;
        }

        const auto accepted = env->CallBooleanMethod(bridge, api.requestMtu, RequestedMtu) == JNI_TRUE;
        jni::takeException(env);

        std::unique_lock lock(state->mutex);

        if (accepted)
            state->changed.wait_for(lock, MtuTimeout, [&] { return state->pendingComplete || !state->connected; });

        state->pendingOperation = Operation::None;
        state->pendingComplete = false;
    }

    const int64_t scanToken;
    const std::shared_ptr<ScanState> scan;
};

}

std::unique_ptr<ICentral> createPlatformCentral()
{
    return std::make_unique<AndroidCentral>();
}

}

// --- JNI entry points ---------------------------------------------------------------------------
//
// Exported Java_... symbols avoid load-time registration. Callbacks resolve weak state tokens.

using opendaq_bt::ble::connectionStateFor;
using opendaq_bt::ble::scanStateFor;

extern "C" JNIEXPORT void JNICALL
Java_com_opendaq_bt_OpenDaqBluetooth_nativeInitialize(JNIEnv* env, jclass, jobject context)
{
    opendaq_bt::ble::jni::setApplicationContext(env, context);

    // This Java thread has the application class loader.
    opendaq_bt::ble::cacheJavaApi(env);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_opendaq_bt_OpenDaqBluetooth_nativeBridgeReady(JNIEnv*, jclass)
{
    return opendaq_bt::ble::api.ready.load(std::memory_order_acquire) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_opendaq_bt_ScanBridge_nativeOnScanResult(JNIEnv* env,
                                                  jclass,
                                                  jlong token,
                                                  jstring address,
                                                  jstring name,
                                                  jint rssi,
                                                  jobjectArray serviceUuids,
                                                  jintArray manufacturerIds,
                                                  jobjectArray manufacturerData)
{
    const auto state = scanStateFor(token);
    if (!state)
        return;

    namespace jni = opendaq_bt::ble::jni;

    opendaq_bt::ble::Advertisement advertisement;
    advertisement.id = jni::toStdString(env, address);
    if (advertisement.id.empty())
        return;

    advertisement.name = jni::toStdString(env, name);
    advertisement.rssi = static_cast<int16_t>(rssi);
    advertisement.lastSeen = std::chrono::steady_clock::now();

    if (serviceUuids != nullptr)
    {
        const auto count = env->GetArrayLength(serviceUuids);
        for (jsize i = 0; i < count; ++i)
        {
            const auto uuid = static_cast<jstring>(env->GetObjectArrayElement(serviceUuids, i));
            advertisement.serviceUuids.push_back(
                opendaq_bt::ble::expandUuid(jni::toStdString(env, uuid)));
            env->DeleteLocalRef(uuid);
        }
    }

    if (manufacturerIds != nullptr && manufacturerData != nullptr)
    {
        const auto count = std::min(env->GetArrayLength(manufacturerIds),
                                    env->GetArrayLength(manufacturerData));

        std::vector<jint> ids(static_cast<size_t>(count));
        if (count > 0)
            env->GetIntArrayRegion(manufacturerIds, 0, count, ids.data());

        for (jsize i = 0; i < count; ++i)
        {
            const auto payload = static_cast<jbyteArray>(env->GetObjectArrayElement(manufacturerData, i));
            advertisement.manufacturerData[static_cast<uint16_t>(ids[static_cast<size_t>(i)])] =
                jni::toBytes(env, payload);
            env->DeleteLocalRef(payload);
        }
    }

    std::scoped_lock lock(state->mutex);
    auto& stored = state->discovered[advertisement.id];

    // Merge sparse advertisement and scan-response packets.
    if (advertisement.name.empty())
        advertisement.name = stored.name;
    if (advertisement.serviceUuids.empty())
        advertisement.serviceUuids = stored.serviceUuids;
    if (advertisement.manufacturerData.empty())
        advertisement.manufacturerData = stored.manufacturerData;

    stored = std::move(advertisement);
}

extern "C" JNIEXPORT void JNICALL
Java_com_opendaq_bt_ScanBridge_nativeOnScanFailed(JNIEnv*, jclass, jlong /*token*/, jint /*errorCode*/)
{
    // ICentral has no asynchronous scan-error channel.
}

extern "C" JNIEXPORT void JNICALL
Java_com_opendaq_bt_GattBridge_nativeOnConnectionStateChange(JNIEnv*,
                                                             jclass,
                                                             jlong token,
                                                             jint status,
                                                             jint newState)
{
    const auto state = connectionStateFor(token);
    if (!state)
        return;

    std::function<void()> disconnected;

    {
        std::scoped_lock lock(state->mutex);

        const auto wasConnected = state->connected;

        state->connected = newState == opendaq_bt::ble::StateConnected;
        state->connectionSettled = true;
        state->connectionStatus = status;

        if (wasConnected && !state->connected)
            disconnected = state->disconnectCallback;
    }

    state->changed.notify_all();

    // User callbacks run outside the state lock.
    if (disconnected)
        disconnected();
}

extern "C" JNIEXPORT void JNICALL
Java_com_opendaq_bt_GattBridge_nativeOnServicesDiscovered(JNIEnv*, jclass, jlong token, jint status)
{
    const auto state = connectionStateFor(token);
    if (!state)
        return;

    {
        std::scoped_lock lock(state->mutex);
        state->servicesResolved = true;
        state->servicesStatus = status;
    }

    state->changed.notify_all();
}

extern "C" JNIEXPORT void JNICALL
Java_com_opendaq_bt_GattBridge_nativeOnMtuChanged(JNIEnv*, jclass, jlong token, jint mtu, jint status)
{
    const auto state = connectionStateFor(token);
    if (!state)
        return;

    {
        std::scoped_lock lock(state->mutex);

        if (status == opendaq_bt::ble::GattSuccess && mtu > 0)
            state->mtu = static_cast<uint16_t>(mtu);

        if (state->pendingOperation == opendaq_bt::ble::Operation::Mtu)
        {
            state->pendingStatus = status;
            state->pendingComplete = true;
        }
    }

    state->changed.notify_all();
}

namespace
{

void completeOperation(const std::shared_ptr<opendaq_bt::ble::ConnectionState>& state,
                       opendaq_bt::ble::Operation operation,
                       const std::string& handle,
                       int status,
                       std::vector<uint8_t> value = {})
{
    {
        std::scoped_lock lock(state->mutex);

        // Ignore stale replies from timed-out operations.
        if (state->pendingOperation != operation || state->pendingHandle != handle)
            return;

        state->pendingStatus = status;
        state->pendingValue = std::move(value);
        state->pendingComplete = true;
    }

    state->changed.notify_all();
}

}

extern "C" JNIEXPORT void JNICALL
Java_com_opendaq_bt_GattBridge_nativeOnCharacteristicRead(JNIEnv* env,
                                                          jclass,
                                                          jlong token,
                                                          jstring handle,
                                                          jbyteArray value,
                                                          jint status)
{
    const auto state = connectionStateFor(token);
    if (!state)
        return;

    namespace jni = opendaq_bt::ble::jni;
    completeOperation(state, opendaq_bt::ble::Operation::Read, jni::toStdString(env, handle),
                      status, jni::toBytes(env, value));
}

extern "C" JNIEXPORT void JNICALL
Java_com_opendaq_bt_GattBridge_nativeOnCharacteristicWrite(JNIEnv* env,
                                                           jclass,
                                                           jlong token,
                                                           jstring handle,
                                                           jint status)
{
    const auto state = connectionStateFor(token);
    if (!state)
        return;

    completeOperation(state, opendaq_bt::ble::Operation::Write,
                      opendaq_bt::ble::jni::toStdString(env, handle), status);
}

extern "C" JNIEXPORT void JNICALL
Java_com_opendaq_bt_GattBridge_nativeOnDescriptorWrite(JNIEnv* env,
                                                       jclass,
                                                       jlong token,
                                                       jstring handle,
                                                       jint status)
{
    const auto state = connectionStateFor(token);
    if (!state)
        return;

    completeOperation(state, opendaq_bt::ble::Operation::DescriptorWrite,
                      opendaq_bt::ble::jni::toStdString(env, handle), status);
}

extern "C" JNIEXPORT void JNICALL
Java_com_opendaq_bt_GattBridge_nativeOnCharacteristicChanged(JNIEnv* env,
                                                             jclass,
                                                             jlong token,
                                                             jstring handle,
                                                             jbyteArray value)
{
    const auto state = connectionStateFor(token);
    if (!state)
        return;

    namespace jni = opendaq_bt::ble::jni;

    const auto key = jni::toStdString(env, handle);
    auto payload = jni::toBytes(env, value);

    opendaq_bt::ble::NotificationCallback callback;

    {
        std::scoped_lock lock(state->mutex);
        const auto it = state->subscriptions.find(key);
        if (it == state->subscriptions.end())
            return;

        // Copy before unlocking so unsubscribe cannot invalidate it.
        callback = it->second;
    }

    if (callback)
        callback(payload);
}

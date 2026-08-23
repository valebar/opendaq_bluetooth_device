// BLE backend for macOS, built on CoreBluetooth.
//
// VERIFIED: this translation unit compiles clean on Apple clang (arm64, macOS 15), and scanning,
// connecting, GATT resolution, characteristic reads and notification delivery have all been run
// against a live peripheral. Characteristic writes, and two peripherals connected at once, are
// implemented but have not yet been exercised against real hardware.
//
// Four CoreBluetooth particulars shape the code:
//
//  1. CBCentralManager is driven by a delegate on a dispatch queue. Passing our own queue rather
//     than nil is essential: a nil queue means the main queue, which would require the host
//     application to be running a main run loop -- an unreasonable demand from a plugin.
//  2. Peripherals are identified by an NSUUID that Apple generates per host. The hardware MAC is
//     deliberately never exposed, which is why connection strings are documented as host-specific.
//  3. Nothing works until the manager reports CBManagerStatePoweredOn, and on macOS 11+ the host
//     application bundle must declare NSBluetoothAlwaysUsageDescription or the state settles on
//     CBManagerStateUnauthorized instead.
//  4. One CBCentralManager and one delegate serve every peripheral, and the delegate methods are
//     the only place the peripheral an event belongs to is known. Everything per-peripheral
//     therefore lives in a PeripheralState block routed by identifier -- see the comment there.

#include <bluetooth_module/ble/ble_central.h>

#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace opendaq_bt::ble;
using namespace std::chrono;

namespace
{

std::string toStdString(NSString* value)
{
    return value == nil ? std::string{} : std::string([value UTF8String]);
}

/*!
 * CBUUID prints 16-bit UUIDs in short form; expand to the full 128-bit spelling so identifiers
 * compare equal regardless of which end produced them.
 */
std::string uuidToString(CBUUID* uuid)
{
    const auto text = toStdString([uuid UUIDString]);
    return expandUuid(text);
}

std::vector<uint8_t> toBytes(NSData* data)
{
    if (data == nil)
        return {};

    const auto* first = static_cast<const uint8_t*>([data bytes]);
    return std::vector<uint8_t>(first, first + [data length]);
}

uint32_t mapProperties(CBCharacteristicProperties properties)
{
    uint32_t result = CharPropNone;

    if (properties & CBCharacteristicPropertyRead)
        result |= CharPropRead;
    if (properties & CBCharacteristicPropertyWrite)
        result |= CharPropWrite;
    if (properties & CBCharacteristicPropertyWriteWithoutResponse)
        result |= CharPropWriteWithoutResponse;
    if (properties & CBCharacteristicPropertyNotify)
        result |= CharPropNotify;
    if (properties & CBCharacteristicPropertyIndicate)
        result |= CharPropIndicate;
    if (properties & CBCharacteristicPropertyBroadcast)
        result |= CharPropBroadcast;
    if (properties & CBCharacteristicPropertyAuthenticatedSignedWrites)
        result |= CharPropAuthenticatedSignedWrites;
    if (properties & CBCharacteristicPropertyExtendedProperties)
        result |= CharPropExtendedProperties;

    return result;
}

/*!
 * State of one connected peripheral, shared between its IConnection and the delegate.
 *
 * This is deliberately per-peripheral rather than per-central. Every map below is keyed by a
 * characteristic handle, which is only unique *within* a peripheral: two heart rate straps both
 * key their measurement characteristic as "0000180d-.../00002a37-...". Sharing one block between
 * them would mean the second subscription overwriting the first, both peripherals' notifications
 * landing on whichever callback registered last, and one device disconnecting marking the others
 * disconnected too.
 *
 * Held by shared_ptr because delegate callbacks run on the dispatch queue and may still be in
 * flight while the connection that owns the block is being destroyed.
 */
struct PeripheralState
{
    std::mutex mutex;
    std::condition_variable changed;

    // Connection handshake. Per-peripheral, so connecting to a second peripheral while the first
    // is still resolving its GATT database cannot disturb it.
    bool connectFinished = false;
    bool connectSucceeded = false;
    std::string connectError;

    int pendingServiceDiscoveries = 0;
    bool servicesDiscovered = false;

    bool connected = false;

    std::map<std::string, std::vector<uint8_t>> readValues;
    std::map<std::string, std::string> readErrors;
    std::map<std::string, bool> readCompleted;
    std::map<std::string, bool> writeCompleted;
    std::map<std::string, std::string> writeErrors;

    std::function<void()> disconnectCallback;
    std::map<std::string, NotificationCallback> notificationCallbacks;
};

/*! Central-wide state: the adapter, the scan, and the table of connected peripherals. */
struct SharedState
{
    std::mutex mutex;
    std::condition_variable changed;

    CBManagerState managerState = CBManagerStateUnknown;
    std::map<std::string, Advertisement> discovered;

    /*! Keyed by the peripheral's CoreBluetooth identifier, which is what routes every callback. */
    std::map<std::string, std::shared_ptr<PeripheralState>> peripherals;
};

std::string handleFor(CBCharacteristic* characteristic)
{
    // Service UUID plus characteristic UUID uniquely addresses a characteristic within a
    // peripheral for our purposes, and unlike a CBCharacteristic* it is safely copyable.
    return uuidToString([[characteristic service] UUID]) + "/" + uuidToString([characteristic UUID]);
}

/*! The per-host NSUUID CoreBluetooth assigns a peripheral; also its connection-string identifier. */
std::string identifierOf(CBPeripheral* peripheral)
{
    return peripheral == nil ? std::string{} : toStdString([[peripheral identifier] UUIDString]);
}

/*!
 * Routes a delegate callback to the peripheral it belongs to, or returns nullptr when the
 * connection has already been torn down -- CoreBluetooth can deliver a callback after the last
 * reference to a peripheral is gone.
 */
std::shared_ptr<PeripheralState> stateOf(SharedState* state, CBPeripheral* peripheral)
{
    if (state == nullptr || peripheral == nil)
        return nullptr;

    std::scoped_lock lock(state->mutex);

    const auto it = state->peripherals.find(identifierOf(peripheral));
    return it == state->peripherals.end() ? nullptr : it->second;
}

}

// ---------------------------------------------------------------------------------------------

@interface OpenDaqBleDelegate : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
@property (nonatomic, assign) SharedState* state;
@end

@implementation OpenDaqBleDelegate

- (void)centralManagerDidUpdateState:(CBCentralManager*)central
{
    std::scoped_lock lock(self.state->mutex);
    self.state->managerState = [central state];
    self.state->changed.notify_all();
}

- (void)centralManager:(CBCentralManager*)central
 didDiscoverPeripheral:(CBPeripheral*)peripheral
     advertisementData:(NSDictionary<NSString*, id>*)advertisementData
                  RSSI:(NSNumber*)RSSI
{
    Advertisement advertisement;
    advertisement.id = toStdString([[peripheral identifier] UUIDString]);
    advertisement.rssi = static_cast<int16_t>([RSSI intValue]);
    advertisement.lastSeen = steady_clock::now();

    NSString* localName = advertisementData[CBAdvertisementDataLocalNameKey];
    advertisement.name = toStdString(localName != nil ? localName : [peripheral name]);

    NSArray<CBUUID*>* uuids = advertisementData[CBAdvertisementDataServiceUUIDsKey];
    for (CBUUID* uuid in uuids)
        advertisement.serviceUuids.push_back(uuidToString(uuid));

    NSData* manufacturerData = advertisementData[CBAdvertisementDataManufacturerDataKey];
    if (manufacturerData != nil && [manufacturerData length] >= 2)
    {
        const auto bytes = toBytes(manufacturerData);
        // The first two octets are the little-endian SIG company identifier.
        const uint16_t companyId = static_cast<uint16_t>(bytes[0]) |
                                   static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8);
        advertisement.manufacturerData[companyId] =
            std::vector<uint8_t>(bytes.begin() + 2, bytes.end());
    }

    std::scoped_lock lock(self.state->mutex);

    // Name and service list often arrive only in a later scan response; merge instead of replace.
    auto& stored = self.state->discovered[advertisement.id];
    if (advertisement.name.empty())
        advertisement.name = stored.name;
    if (advertisement.serviceUuids.empty())
        advertisement.serviceUuids = stored.serviceUuids;
    if (advertisement.manufacturerData.empty())
        advertisement.manufacturerData = stored.manufacturerData;

    stored = std::move(advertisement);
}

- (void)centralManager:(CBCentralManager*)central didConnectPeripheral:(CBPeripheral*)peripheral
{
    const auto peripheralState = stateOf(self.state, peripheral);
    if (!peripheralState)
        return;

    {
        std::scoped_lock lock(peripheralState->mutex);
        peripheralState->connectFinished = true;
        peripheralState->connectSucceeded = true;
        peripheralState->connected = true;
    }
    peripheralState->changed.notify_all();
}

- (void)centralManager:(CBCentralManager*)central
didFailToConnectPeripheral:(CBPeripheral*)peripheral
                 error:(NSError*)error
{
    const auto peripheralState = stateOf(self.state, peripheral);
    if (!peripheralState)
        return;

    {
        std::scoped_lock lock(peripheralState->mutex);
        peripheralState->connectFinished = true;
        peripheralState->connectSucceeded = false;
        peripheralState->connectError = toStdString([error localizedDescription]);
    }
    peripheralState->changed.notify_all();
}

- (void)centralManager:(CBCentralManager*)central
didDisconnectPeripheral:(CBPeripheral*)peripheral
                 error:(NSError*)error
{
    const auto peripheralState = stateOf(self.state, peripheral);
    if (!peripheralState)
        return;

    std::function<void()> callback;
    {
        std::scoped_lock lock(peripheralState->mutex);
        peripheralState->connected = false;
        callback = peripheralState->disconnectCallback;
    }
    peripheralState->changed.notify_all();

    if (callback)
        callback();
}

- (void)peripheral:(CBPeripheral*)peripheral didDiscoverServices:(NSError*)error
{
    const auto peripheralState = stateOf(self.state, peripheral);
    if (!peripheralState)
        return;

    if (error != nil || [peripheral services] == nil)
    {
        {
            std::scoped_lock lock(peripheralState->mutex);
            peripheralState->servicesDiscovered = true;
        }
        peripheralState->changed.notify_all();
        return;
    }

    {
        std::scoped_lock lock(peripheralState->mutex);
        peripheralState->pendingServiceDiscoveries = static_cast<int>([[peripheral services] count]);
    }

    for (CBService* service in [peripheral services])
        [peripheral discoverCharacteristics:nil forService:service];
}

- (void)peripheral:(CBPeripheral*)peripheral
didDiscoverCharacteristicsForService:(CBService*)service
             error:(NSError*)error
{
    const auto peripheralState = stateOf(self.state, peripheral);
    if (!peripheralState)
        return;

    {
        std::scoped_lock lock(peripheralState->mutex);
        if (--peripheralState->pendingServiceDiscoveries <= 0)
            peripheralState->servicesDiscovered = true;
    }
    peripheralState->changed.notify_all();
}

- (void)peripheral:(CBPeripheral*)peripheral
didUpdateValueForCharacteristic:(CBCharacteristic*)characteristic
             error:(NSError*)error
{
    const auto peripheralState = stateOf(self.state, peripheral);
    if (!peripheralState)
        return;

    const auto handle = handleFor(characteristic);

    // CoreBluetooth funnels reads *and* notifications through this one delegate method, with
    // nothing in the arguments to tell them apart. An outstanding read is therefore taken to own
    // the next update for that characteristic; anything else is a notification. A notification
    // landing in the same instant as a read would be attributed to the read, which is why reads
    // and subscriptions to the same characteristic should not be issued concurrently.
    bool completesPendingRead = false;
    NotificationCallback callback;
    {
        std::scoped_lock lock(peripheralState->mutex);

        const auto pending = peripheralState->readCompleted.find(handle);
        completesPendingRead = pending != peripheralState->readCompleted.end() && !pending->second;

        if (completesPendingRead)
        {
            if (error != nil)
                peripheralState->readErrors[handle] = toStdString([error localizedDescription]);
            else
                peripheralState->readValues[handle] = toBytes([characteristic value]);
            peripheralState->readCompleted[handle] = true;
        }
        else if (error == nil)
        {
            const auto it = peripheralState->notificationCallbacks.find(handle);
            if (it != peripheralState->notificationCallbacks.end())
                callback = it->second;
        }
    }

    if (completesPendingRead)
    {
        peripheralState->changed.notify_all();
        return;
    }

    if (callback)
        callback(toBytes([characteristic value]));
}

- (void)peripheral:(CBPeripheral*)peripheral
didWriteValueForCharacteristic:(CBCharacteristic*)characteristic
             error:(NSError*)error
{
    const auto peripheralState = stateOf(self.state, peripheral);
    if (!peripheralState)
        return;

    const auto handle = handleFor(characteristic);
    {
        std::scoped_lock lock(peripheralState->mutex);
        if (error != nil)
            peripheralState->writeErrors[handle] = toStdString([error localizedDescription]);
        peripheralState->writeCompleted[handle] = true;
    }
    peripheralState->changed.notify_all();
}

@end

// ---------------------------------------------------------------------------------------------

namespace
{

class CoreBluetoothConnection final : public IConnection
{
public:
    CoreBluetoothConnection(CBCentralManager* manager,
                            CBPeripheral* peripheral,
                            std::shared_ptr<SharedState> central,
                            std::shared_ptr<PeripheralState> state,
                            std::vector<Service> services,
                            std::map<std::string, CBCharacteristic*> characteristics)
        : manager(manager)
        , peripheral(peripheral)
        , identifier(identifierOf(peripheral))
        , central(std::move(central))
        , state(std::move(state))
        , resolvedServices(std::move(services))
        , characteristicsByHandle(std::move(characteristics))
    {
    }

    ~CoreBluetoothConnection() override
    {
        // Order matters. Clearing the callbacks first means a delegate callback already holding a
        // reference to this state block finds nothing to call; deregistering second means any
        // callback that has not yet looked the block up never finds it at all.
        {
            std::scoped_lock lock(state->mutex);
            state->disconnectCallback = nullptr;
            state->notificationCallbacks.clear();
        }

        if (central)
        {
            std::scoped_lock lock(central->mutex);

            // Only if the table still points at *this* block: connecting twice to one peripheral
            // replaces the entry, and the older connection must not evict the newer one's state.
            const auto it = central->peripherals.find(identifier);
            if (it != central->peripherals.end() && it->second == state)
                central->peripherals.erase(it);
        }

        if (manager != nil && peripheral != nil)
            [manager cancelPeripheralConnection:peripheral];
    }

    bool isConnected() const override
    {
        std::scoped_lock lock(state->mutex);
        return state->connected;
    }

    const std::vector<Service>& services() const override { return resolvedServices; }

    uint16_t mtu() const override
    {
        if (peripheral == nil)
            return 0;

        // CoreBluetooth reports the usable payload; add the 3-byte ATT header for a true MTU.
        const auto payload =
            [peripheral maximumWriteValueLengthForType:CBCharacteristicWriteWithoutResponse];

        return static_cast<uint16_t>(payload + 3);
    }

    std::vector<uint8_t> read(const std::string& handle) override
    {
        CBCharacteristic* characteristic = lookup(handle);

        std::unique_lock lock(state->mutex);
        state->readCompleted[handle] = false;
        state->readErrors.erase(handle);
        lock.unlock();

        [peripheral readValueForCharacteristic:characteristic];

        lock.lock();
        if (!state->changed.wait_for(lock, seconds(10), [&] { return state->readCompleted[handle]; }))
            throw BleError("Timed out reading " + handle);

        const auto errorIt = state->readErrors.find(handle);
        if (errorIt != state->readErrors.end())
            throw BleError("Reading " + handle + " failed: " + errorIt->second);

        return state->readValues[handle];
    }

    void write(const std::string& handle, const std::vector<uint8_t>& data, bool withResponse) override
    {
        CBCharacteristic* characteristic = lookup(handle);

        NSData* payload = [NSData dataWithBytes:data.data() length:data.size()];
        const auto type =
            withResponse ? CBCharacteristicWriteWithResponse : CBCharacteristicWriteWithoutResponse;

        if (!withResponse)
        {
            // Unacknowledged writes never produce a delegate callback, so there is nothing to await.
            [peripheral writeValue:payload forCharacteristic:characteristic type:type];
            return;
        }

        std::unique_lock lock(state->mutex);
        state->writeCompleted[handle] = false;
        state->writeErrors.erase(handle);
        lock.unlock();

        [peripheral writeValue:payload forCharacteristic:characteristic type:type];

        lock.lock();
        if (!state->changed.wait_for(lock, seconds(10), [&] { return state->writeCompleted[handle]; }))
            throw BleError("Timed out writing " + handle);

        const auto errorIt = state->writeErrors.find(handle);
        if (errorIt != state->writeErrors.end())
            throw BleError("Writing " + handle + " failed: " + errorIt->second);
    }

    void subscribe(const std::string& handle, NotificationCallback callback) override
    {
        CBCharacteristic* characteristic = lookup(handle);

        {
            std::scoped_lock lock(state->mutex);
            state->notificationCallbacks[handle] = std::move(callback);
        }

        [peripheral setNotifyValue:YES forCharacteristic:characteristic];
    }

    void unsubscribe(const std::string& handle) override
    {
        {
            std::scoped_lock lock(state->mutex);
            state->notificationCallbacks.erase(handle);
        }

        const auto it = characteristicsByHandle.find(handle);
        if (it == characteristicsByHandle.end())
            return;

        [peripheral setNotifyValue:NO forCharacteristic:it->second];
    }

    void setDisconnectCallback(std::function<void()> callback) override
    {
        std::scoped_lock lock(state->mutex);
        state->disconnectCallback = std::move(callback);
    }

private:
    CBCharacteristic* lookup(const std::string& handle)
    {
        const auto it = characteristicsByHandle.find(handle);
        if (it == characteristicsByHandle.end())
            throw BleError("No such characteristic: " + handle);

        if (!isConnected())
            throw BleError("Device is disconnected.");

        return it->second;
    }

    CBCentralManager* manager = nil;
    CBPeripheral* peripheral = nil;
    const std::string identifier;
    std::shared_ptr<SharedState> central;
    std::shared_ptr<PeripheralState> state;
    std::vector<Service> resolvedServices;
    std::map<std::string, CBCharacteristic*> characteristicsByHandle;
};

class CoreBluetoothCentral final : public ICentral
{
public:
    CoreBluetoothCentral()
        : state(std::make_shared<SharedState>())
    {
        queue = dispatch_queue_create("com.pawsm.opendaq.bluetooth", DISPATCH_QUEUE_SERIAL);

        delegate = [[OpenDaqBleDelegate alloc] init];
        delegate.state = state.get();

        // Our own queue, never nil: a nil queue would bind CoreBluetooth to the main run loop,
        // which a module loaded into an arbitrary host application cannot assume exists.
        manager = [[CBCentralManager alloc] initWithDelegate:delegate queue:queue];
    }

    ~CoreBluetoothCentral() override { stopScan(); }

    bool isAvailable(std::string& reason) override
    {
        if (!waitForPoweredOn(seconds(3)))
        {
            std::scoped_lock lock(state->mutex);
            switch (state->managerState)
            {
                case CBManagerStateUnauthorized:
                    reason = "this application is not authorised to use Bluetooth. On macOS 11 and "
                             "later the host application bundle must declare "
                             "NSBluetoothAlwaysUsageDescription, and the user must allow Bluetooth "
                             "access when prompted";
                    break;
                case CBManagerStatePoweredOff:
                    reason = "Bluetooth is turned off";
                    break;
                case CBManagerStateUnsupported:
                    reason = "this Mac does not support Bluetooth Low Energy";
                    break;
                default:
                    reason = "the Bluetooth manager did not become ready";
                    break;
            }
            return false;
        }

        return true;
    }

    void startScan() override
    {
        std::string reason;
        if (!isAvailable(reason))
            throw BleError("Cannot start a Bluetooth scan: " + reason);

        if (scanning)
            return;

        // Duplicate keys keep RSSI current instead of reporting each peripheral only once.
        NSDictionary* options = @{CBCentralManagerScanOptionAllowDuplicatesKey: @YES};
        [manager scanForPeripheralsWithServices:nil options:options];
        scanning = true;
    }

    void stopScan() override
    {
        if (!scanning)
            return;

        [manager stopScan];
        scanning = false;
    }

    bool isScanning() const override { return scanning; }

    void setServicesOfInterest(const std::vector<std::string>& uuids) override
    {
        std::scoped_lock lock(state->mutex);
        servicesOfInterest = uuids;
    }

    std::vector<Advertisement> getDiscovered() override
    {
        // A peripheral this Mac is connected to has stopped advertising, so didDiscoverPeripheral
        // will never fire for it again -- and it is the one peripheral we are certain we can
        // reach. CoreBluetooth can only be asked about connected peripherals by service, which is
        // what setServicesOfInterest is for.
        auto linked = connectedPeripherals();

        std::scoped_lock lock(state->mutex);

        std::vector<Advertisement> result;
        result.reserve(state->discovered.size() + linked.size());

        for (const auto& [id, advertisement] : state->discovered)
        {
            auto merged = advertisement;

            const auto it = linked.find(id);
            if (it != linked.end())
            {
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

        NSUUID* uuid = [[NSUUID alloc] initWithUUIDString:@(id.c_str())];
        if (uuid == nil)
            throw BleError("\"" + id + "\" is not a CoreBluetooth peripheral identifier.");

        NSArray<CBPeripheral*>* known =
            [manager retrievePeripheralsWithIdentifiers:@[uuid]];
        if ([known count] == 0)
            throw BleError("Bluetooth device " + id +
                           " is not known to this Mac. Run a discovery scan first so CoreBluetooth "
                           "can find it.");

        CBPeripheral* peripheral = known[0];
        [peripheral setDelegate:delegate];

        // Registered before the connect is issued, because that is what routes the delegate
        // callbacks it produces back here. Connecting to a second peripheral while this one is
        // still resolving touches a different block entirely.
        // Keyed by what CoreBluetooth itself reports, not by the caller's `id`: the two agree
        // today, but only because the connection-string parser happens to normalise a UUID the
        // same way NSUUID prints one. A callback that cannot find its block is a silent hang.
        const auto key = identifierOf(peripheral);

        const auto peripheralState = std::make_shared<PeripheralState>();
        {
            std::scoped_lock lock(state->mutex);
            state->peripherals[key] = peripheralState;
        }

        // Any failure from here on has to take the entry back out again: nothing else owns it
        // until the IConnection below is handed to the caller.
        const auto abandon = [this, &key, &peripheralState, peripheral] {
            {
                std::scoped_lock lock(state->mutex);
                const auto it = state->peripherals.find(key);
                if (it != state->peripherals.end() && it->second == peripheralState)
                    state->peripherals.erase(it);
            }

            [manager cancelPeripheralConnection:peripheral];
        };

        [manager connectPeripheral:peripheral options:nil];

        {
            std::unique_lock lock(peripheralState->mutex);
            if (!peripheralState->changed.wait_for(
                    lock, timeout, [&] { return peripheralState->connectFinished; }))
            {
                lock.unlock();
                abandon();
                throw BleError("Timed out connecting to " + id + ".");
            }

            if (!peripheralState->connectSucceeded)
            {
                const auto error = peripheralState->connectError;
                lock.unlock();
                abandon();
                throw BleError("Cannot connect to " + id + ": " + error);
            }
        }

        [peripheral discoverServices:nil];

        {
            std::unique_lock lock(peripheralState->mutex);
            if (!peripheralState->changed.wait_for(
                    lock, timeout, [&] { return peripheralState->servicesDiscovered; }))
            {
                lock.unlock();
                abandon();
                throw BleError("Timed out reading the GATT database of " + id +
                               ". The peripheral may require pairing.");
            }
        }

        std::vector<Service> services;
        std::map<std::string, CBCharacteristic*> characteristicsByHandle;

        for (CBService* cbService in [peripheral services])
        {
            Service service;
            service.uuid = uuidToString([cbService UUID]);
            service.primary = ![cbService isPrimary] ? false : true;

            for (CBCharacteristic* cbCharacteristic in [cbService characteristics])
            {
                Characteristic characteristic;
                characteristic.uuid = uuidToString([cbCharacteristic UUID]);
                characteristic.handle = handleFor(cbCharacteristic);
                characteristic.properties = mapProperties([cbCharacteristic properties]);

                characteristicsByHandle[characteristic.handle] = cbCharacteristic;
                service.characteristics.push_back(std::move(characteristic));
            }

            services.push_back(std::move(service));
        }

        return std::make_unique<CoreBluetoothConnection>(manager,
                                                        peripheral,
                                                        state,
                                                        peripheralState,
                                                        std::move(services),
                                                        std::move(characteristicsByHandle));
    }

private:
    /*!
     * Peripherals this Mac holds a link to, keyed by identifier.
     *
     * retrieveConnectedPeripheralsWithServices: takes a non-empty service array and has no
     * "everything" form, so this reports only peripherals hosting a service the module has a
     * profile for -- even when discovery filtering is turned off. Asking once per service rather
     * than passing the whole array in one call is what keeps serviceUuids honest: a peripheral
     * returned for a batch matched at least one of the UUIDs, and CoreBluetooth does not say
     * which.
     */
    std::map<std::string, Advertisement> connectedPeripherals()
    {
        std::vector<std::string> interesting;
        {
            std::scoped_lock lock(state->mutex);

            // Asking CoreBluetooth anything before it is powered on is an API misuse it logs
            // about, and the answer would be empty regardless.
            if (state->managerState != CBManagerStatePoweredOn)
                return {};

            interesting = servicesOfInterest;
        }

        std::map<std::string, Advertisement> found;

        for (const auto& uuid : interesting)
        {
            // UUIDWithString: raises rather than returning nil on a malformed string, and an
            // Objective-C exception unwinding through the C++ frames above would be fatal. The
            // registry only ever yields the expanded form, so anything else is a bug to skip.
            if (uuid.size() != 36)
                continue;

            CBUUID* serviceUuid =
                [CBUUID UUIDWithString:[NSString stringWithUTF8String:uuid.c_str()]];
            if (serviceUuid == nil)
                continue;

            NSArray<CBPeripheral*>* peripherals =
                [manager retrieveConnectedPeripheralsWithServices:@[serviceUuid]];

            for (CBPeripheral* peripheral in peripherals)
            {
                auto& advertisement = found[toStdString([[peripheral identifier] UUIDString])];

                if (advertisement.id.empty())
                {
                    advertisement.id = toStdString([[peripheral identifier] UUIDString]);
                    advertisement.name = toStdString([peripheral name]);
                    advertisement.connected = true;

                    // Not advertising, so there is no reading to report; Advertisement documents
                    // rssi 0 as "unknown" for exactly this case.
                    advertisement.rssi = 0;
                    advertisement.lastSeen = steady_clock::now();
                }

                advertisement.serviceUuids.push_back(uuidToString(serviceUuid));
            }
        }

        return found;
    }

    bool waitForPoweredOn(seconds timeout)
    {
        std::unique_lock lock(state->mutex);
        return state->changed.wait_for(lock, timeout, [&] {
            return state->managerState == CBManagerStatePoweredOn;
        });
    }

    std::shared_ptr<SharedState> state;

    // What the openDAQ layer told us it has profiles for; see connectedPeripherals().
    std::vector<std::string> servicesOfInterest;
    dispatch_queue_t queue = nil;
    OpenDaqBleDelegate* delegate = nil;
    CBCentralManager* manager = nil;
    volatile bool scanning = false;
};

}

namespace opendaq_bt::ble
{

std::unique_ptr<ICentral> createPlatformCentral()
{
    return std::make_unique<CoreBluetoothCentral>();
}

}

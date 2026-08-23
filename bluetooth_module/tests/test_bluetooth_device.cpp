#include "fake_ble_central.h"

#include <bluetooth_module/bluetooth_device.h>
#include <bluetooth_module/bluetooth_module.h>

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include <opendaq/context_factory.h>
#include <opendaq/search_filter_factory.h>

using namespace daq;
using namespace daq::modules::bluetooth_module;
namespace ble = opendaq_bt::ble;

namespace
{
/*! Module defaults wait 2s for the radio to warm up; a scripted backend needs none of that. */
PropertyObjectPtr instantConfig()
{
    auto config = BluetoothModule::createDefaultConfig();
    config.setPropertyValue("ScanWarmupMs", 0);
    return config;
}
}

using opendaq_bt::testing::FakeCentral;
using opendaq_bt::testing::FakeConnection;

namespace
{

constexpr char BatteryLevelHandle[] = "/fake/service0/char0";
constexpr char UartRxHandle[] = "/fake/service0/char1";
constexpr char SecretHandle[] = "/fake/service1/char0";

FakeCentral::Peripheral peripheralUnderTest()
{
    ble::Characteristic level;
    level.uuid = "00002a19-0000-1000-8000-00805f9b34fb";  // Battery Level
    level.handle = BatteryLevelHandle;
    level.properties = ble::CharPropRead | ble::CharPropNotify;

    ble::Characteristic rx;
    rx.uuid = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";  // Nordic UART RX
    rx.handle = UartRxHandle;
    rx.properties = ble::CharPropWrite;

    ble::Service battery;
    battery.uuid = "0000180f-0000-1000-8000-00805f9b34fb";
    battery.primary = true;
    battery.characteristics = {level, rx};

    ble::Characteristic secret;
    secret.uuid = "12345678-1234-1234-1234-123456789abc";
    secret.handle = SecretHandle;
    secret.properties = ble::CharPropRead;

    ble::Service vendor;
    vendor.uuid = "87654321-4321-4321-4321-cba987654321";
    vendor.primary = false;
    vendor.characteristics = {secret};

    FakeCentral::Peripheral peripheral;
    peripheral.services = {battery, vendor};
    peripheral.values[BatteryLevelHandle] = {0x5A};
    peripheral.values[UartRxHandle] = {};
    peripheral.values[SecretHandle] = {0xDE, 0xAD};

    return peripheral;
}

struct ConnectedDevice
{
    ContextPtr context;
    ModulePtr module;
    DevicePtr device;
    FakeCentral* central = nullptr;
};

ConnectedDevice connectDevice()
{
    auto owned = std::make_unique<FakeCentral>();
    auto* central = owned.get();
    central->addPeripheral("AA:BB:CC:DD:EE:FF", peripheralUnderTest());

    const auto context = NullContext();
    const auto module =
        createWithImplementation<IModule, BluetoothModule>(context, std::move(owned), instantConfig());

    return {context, module, module.createDevice("bluetooth://AA:BB:CC:DD:EE:FF", nullptr), central};
}

/*! The property paths of the two services the peripheral under test exposes. */
constexpr char BatteryService[] = "Battery Service";
constexpr char VendorService[] = "87654321";

constexpr char BatteryLevel[] = "Battery Service.Battery Level";
constexpr char UartRx[] = "Battery Service.Nordic UART RX";
constexpr char Secret[] = "87654321.12345678";

/*! Every object property on the device: one per GATT service. */
ListPtr<IProperty> serviceProperties(const DevicePtr& device)
{
    auto services = List<IProperty>();
    for (const auto& property : device.getAllProperties())
    {
        if (property.getValueType() == ctObject)
            services.pushBack(property);
    }
    return services;
}

/*! The characteristic properties of one service, in declaration order. */
ListPtr<IProperty> characteristicProperties(const DevicePtr& device, const std::string& service)
{
    const PropertyObjectPtr serviceObj = device.getPropertyValue(service);
    return serviceObj.getAllProperties();
}

std::string valueOf(const DevicePtr& device, const std::string& path)
{
    const BaseObjectPtr value = device.getPropertyValue(path);
    return value.assigned() ? static_cast<std::string>(value.toString()) : std::string();
}

/*!
 * Waits for a predicate.
 *
 * A property read serves the cached value and schedules the re-read on a background thread, so the
 * value converging is the one thing here that cannot be observed synchronously. Returns as soon as
 * it holds; the timeout only bounds a genuine failure.
 */
template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

}

TEST(BluetoothDevice, ExposesConnectionStateAsProperties)
{
    const auto connected = connectDevice();

    EXPECT_EQ(connected.device.getPropertyValue("Address"), "AA:BB:CC:DD:EE:FF");
    EXPECT_TRUE(static_cast<Bool>(connected.device.getPropertyValue("Connected")));
    EXPECT_EQ(static_cast<Int>(connected.device.getPropertyValue("MtuSize")), 247);
}

TEST(BluetoothDevice, UsesTheAdvertisedAddressAsItsLocalId)
{
    const auto connected = connectDevice();
    EXPECT_EQ(connected.device.getLocalId(), "BLE_AA_BB_CC_DD_EE_FF");
}

TEST(BluetoothDevice, PublishesOneObjectPropertyPerService)
{
    const auto connected = connectDevice();

    const auto services = serviceProperties(connected.device);
    ASSERT_EQ(services.getCount(), 2u);

    EXPECT_EQ(services[0].getName(), BatteryService);
    EXPECT_EQ(services[1].getName(), VendorService);

    // The UUID has nowhere else to live now that a struct field is gone, so it is the description.
    EXPECT_EQ(services[0].getDescription(), "0000180f-0000-1000-8000-00805f9b34fb");
}

TEST(BluetoothDevice, AddsNoStructTypesToTheContext)
{
    const auto connected = connectDevice();

    // The struct types went with the GattDatabase property they described. Pinned so a half-done
    // revert cannot quietly leave the registration behind.
    EXPECT_FALSE(connected.context.getTypeManager().hasType("BluetoothGattService"));
    EXPECT_FALSE(connected.context.getTypeManager().hasType("BluetoothGattCharacteristic"));
}

TEST(BluetoothDevice, NamesCharacteristicsFromTheAssignedNumbersRegistry)
{
    const auto connected = connectDevice();

    const auto characteristics = characteristicProperties(connected.device, BatteryService);
    ASSERT_EQ(characteristics.getCount(), 2u);
    EXPECT_EQ(characteristics[0].getName(), "Battery Level");
    EXPECT_EQ(characteristics[1].getName(), "Nordic UART RX");

    // The UUID and the flags travel in the description, which is the one piece of metadata openDAQ
    // carries on every property.
    const std::string description = characteristics[0].getDescription();
    EXPECT_NE(description.find("00002a19-0000-1000-8000-00805f9b34fb"), std::string::npos);
    EXPECT_NE(description.find("read,notify"), std::string::npos);
}

TEST(BluetoothDevice, KeepsVendorSpecificUuidsDistinguishable)
{
    const auto connected = connectDevice();

    const auto characteristics = characteristicProperties(connected.device, VendorService);
    ASSERT_EQ(characteristics.getCount(), 1u);
    EXPECT_EQ(characteristics[0].getName(), "12345678");
}

TEST(BluetoothDevice, ReadsReadableCharacteristicsWhileBuildingTheTree)
{
    const auto connected = connectDevice();

    // Battery Level has a SIG format, so it is an integer rather than a hex string.
    EXPECT_EQ(static_cast<Int>(connected.device.getPropertyValue(BatteryLevel)), 0x5A);

    // The vendor characteristic has none, so it keeps its bytes.
    EXPECT_EQ(valueOf(connected.device, Secret), "DE AD");
}

TEST(BluetoothDevice, TypesValuesTheBluetoothSigDefines)
{
    auto owned = std::make_unique<FakeCentral>();
    auto* central = owned.get();

    ble::Characteristic manufacturer;
    manufacturer.uuid = "00002a29-0000-1000-8000-00805f9b34fb";
    manufacturer.handle = "/fake/info/char0";
    manufacturer.properties = ble::CharPropRead;

    ble::Service information;
    information.uuid = "0000180a-0000-1000-8000-00805f9b34fb";
    information.characteristics = {manufacturer};

    FakeCentral::Peripheral peripheral;
    peripheral.services = {information};
    peripheral.values["/fake/info/char0"] = {'W', 'H', 'O', 'O', 'P', 0x00};
    central->addPeripheral("AA:BB:CC:DD:EE:FF", std::move(peripheral));

    const auto module = createWithImplementation<IModule, BluetoothModule>(
        NullContext(), std::move(owned), instantConfig());
    const auto device = module.createDevice("bluetooth://AA:BB:CC:DD:EE:FF", nullptr);

    // Text, with the NUL padding a peripheral commonly sends stripped.
    EXPECT_EQ(valueOf(device, "Device Information.Manufacturer Name"), "WHOOP");
}

TEST(BluetoothDevice, ReadingAPropertyRefreshesItInTheBackground)
{
    const auto connected = connectDevice();

    ASSERT_EQ(static_cast<Int>(connected.device.getPropertyValue(BatteryLevel)), 0x5A);

    connected.central->connection()->setValue(BatteryLevelHandle, {0x2A});

    // The read serves the cache -- a property read holds openDAQ's config lock, and nothing
    // holding it may block on the radio -- and schedules the re-read that converges on the new
    // value.
    connected.device.getPropertyValue(BatteryLevel);

    EXPECT_TRUE(waitFor([&] {
        return static_cast<Int>(connected.device.getPropertyValue(BatteryLevel)) == 0x2A;
    }));
}

TEST(BluetoothDevice, PerformsEveryRadioReadOffTheCallingThread)
{
    const auto connected = connectDevice();

    // The initial read pass runs on the caller's thread on purpose -- during construction, before
    // the device is reachable and while no lock is contended -- so only what follows is of
    // interest here.
    const auto initial = connected.central->connection()->readThreads().size();

    connected.central->connection()->setValue(BatteryLevelHandle, {0x2A});
    connected.device.getPropertyValue(BatteryLevel);

    ASSERT_TRUE(waitFor([&] {
        return static_cast<Int>(connected.device.getPropertyValue(BatteryLevel)) == 0x2A;
    }));

    // The structural rule behind the whole design: openDAQ holds the device's config lock across a
    // property read handler, and a backend thread holds the connection's bus mutex. A handler that
    // read from the radio would take those two in the opposite order and deadlock on real
    // hardware. A fake has no bus mutex to hang on, so assert the rule directly instead.
    const auto threads = connected.central->connection()->readThreads();
    ASSERT_GT(threads.size(), initial);

    for (size_t i = initial; i < threads.size(); ++i)
        EXPECT_NE(threads[i], std::this_thread::get_id());
}

TEST(BluetoothDevice, WritingAPropertyReachesThePeripheral)
{
    const auto connected = connectDevice();

    connected.device.setPropertyValue(UartRx, "DE AD BE EF");

    const auto writes = connected.central->connection()->takeWrites();
    ASSERT_EQ(writes.size(), 1u);
    EXPECT_EQ(writes[0].first, UartRxHandle);
    EXPECT_EQ(writes[0].second, std::vector<uint8_t>({0xDE, 0xAD, 0xBE, 0xEF}));
    EXPECT_TRUE(connected.central->connection()->lastWriteHadResponse());

    // What was written is the only knowledge there is of a write-only characteristic's value.
    EXPECT_EQ(valueOf(connected.device, UartRx), "DE AD BE EF");
}

TEST(BluetoothDevice, RefusesANonHexValueForAHexCharacteristic)
{
    const auto connected = connectDevice();

    EXPECT_ANY_THROW(connected.device.setPropertyValue(UartRx, "hello"));
    EXPECT_ANY_THROW(connected.device.setPropertyValue(UartRx, "ABC"));

    EXPECT_TRUE(connected.central->connection()->takeWrites().empty());
}

TEST(BluetoothDevice, MakesACharacteristicWithoutAWriteBitReadOnly)
{
    const auto connected = connectDevice();

    // Battery Level is read|notify, so GATT says it cannot be written and the property says so too.
    EXPECT_THROW(connected.device.setPropertyValue(BatteryLevel, 1), AccessDeniedException);
    EXPECT_TRUE(connected.central->connection()->takeWrites().empty());
}

TEST(BluetoothDevice, DoesNotWriteBackAValueItJustRefreshed)
{
    auto owned = std::make_unique<FakeCentral>();
    auto* central = owned.get();

    // Readable *and* writable, which is the combination that bites: openDAQ fires write handlers
    // for the protected setter too, so a refresh could push what it just read straight back.
    ble::Characteristic both;
    both.uuid = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
    both.handle = "/fake/uart/rx";
    both.properties = ble::CharPropRead | ble::CharPropWrite;

    ble::Service uart;
    uart.uuid = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
    uart.characteristics = {both};

    FakeCentral::Peripheral peripheral;
    peripheral.services = {uart};
    peripheral.values["/fake/uart/rx"] = {0x01};
    central->addPeripheral("AA:BB:CC:DD:EE:FF", std::move(peripheral));

    const auto module = createWithImplementation<IModule, BluetoothModule>(
        NullContext(), std::move(owned), instantConfig());
    const auto device = module.createDevice("bluetooth://AA:BB:CC:DD:EE:FF", nullptr);

    central->connection()->setValue("/fake/uart/rx", {0x02});
    device.getPropertyValue("Nordic UART.Nordic UART RX");

    ASSERT_TRUE(waitFor([&] {
        return valueOf(device, "Nordic UART.Nordic UART RX") == "02";
    }));

    EXPECT_TRUE(central->connection()->takeWrites().empty())
        << "A background refresh wrote the value it had just read back to the peripheral.";
}

TEST(BluetoothDevice, RejectsAnUnknownPropertyPath)
{
    const auto connected = connectDevice();

    EXPECT_FALSE(connected.device.hasProperty("Heart Rate"));
    EXPECT_ANY_THROW(connected.device.getPropertyValue("Battery Service.Nowhere"));
}

TEST(BluetoothDevice, GivesRepeatedCharacteristicUuidsDistinctNames)
{
    auto owned = std::make_unique<FakeCentral>();
    auto* central = owned.get();

    // A peripheral may legitimately expose the same characteristic twice. Addressing used to have
    // to refuse the ambiguity; nesting plus a suffix removes it instead.
    ble::Characteristic first;
    first.uuid = "00002a19-0000-1000-8000-00805f9b34fb";
    first.handle = "/fake/a";
    first.properties = ble::CharPropRead;

    ble::Characteristic second = first;
    second.handle = "/fake/b";

    ble::Service battery;
    battery.uuid = "0000180f-0000-1000-8000-00805f9b34fb";
    battery.characteristics = {first, second};

    FakeCentral::Peripheral peripheral;
    peripheral.services = {battery};
    peripheral.values["/fake/a"] = {0x01};
    peripheral.values["/fake/b"] = {0x02};
    central->addPeripheral("AA:BB:CC:DD:EE:FF", std::move(peripheral));

    const auto module = createWithImplementation<IModule, BluetoothModule>(
        NullContext(), std::move(owned), instantConfig());
    const auto device = module.createDevice("bluetooth://AA:BB:CC:DD:EE:FF", nullptr);

    const auto characteristics = characteristicProperties(device, BatteryService);
    ASSERT_EQ(characteristics.getCount(), 2u);
    EXPECT_EQ(characteristics[0].getName(), "Battery Level");
    EXPECT_EQ(characteristics[1].getName(), "Battery Level 2");

    EXPECT_EQ(static_cast<Int>(device.getPropertyValue("Battery Service.Battery Level")), 1);
    EXPECT_EQ(static_cast<Int>(device.getPropertyValue("Battery Service.Battery Level 2")), 2);
}

TEST(BluetoothDevice, RefusesWritesToStateMirroredFromThePeripheral)
{
    const auto connected = connectDevice();

    // Address reflects the peripheral, so it is a genuinely read-only openDAQ property: writing
    // is refused outright rather than silently ignored, and the value is left intact.
    EXPECT_ANY_THROW(connected.device.setPropertyValue("Address", "00:00:00:00:00:00"));
    EXPECT_EQ(connected.device.getPropertyValue("Address"), "AA:BB:CC:DD:EE:FF");
}

TEST(BluetoothDevice, SurvivesACharacteristicThatRefusesToBeRead)
{
    auto owned = std::make_unique<FakeCentral>();
    auto* central = owned.get();
    central->addPeripheral("AA:BB:CC:DD:EE:FF", peripheralUnderTest());

    const auto module =
        createWithImplementation<IModule, BluetoothModule>(NullContext(), std::move(owned), instantConfig());

    // A characteristic requiring pairing must not abort construction of the whole database.
    const auto device = module.createDevice("bluetooth://AA:BB:CC:DD:EE:FF", nullptr);
    ASSERT_TRUE(device.assigned());

    EXPECT_EQ(serviceProperties(device).getCount(), 2u);
}

TEST(BluetoothDevice, KeepsEachDeviceInAContextIndependent)
{
    // Two peripherals of the same model share every service and characteristic UUID, so anything
    // keyed by UUID alone would collide between them.
    auto owned = std::make_unique<FakeCentral>();
    owned->addPeripheral("AA:BB:CC:DD:EE:FF", peripheralUnderTest());
    owned->addPeripheral("11:22:33:44:55:66", peripheralUnderTest());

    const auto context = NullContext();
    const auto module =
        createWithImplementation<IModule, BluetoothModule>(context, std::move(owned), instantConfig());

    const auto first = module.createDevice("bluetooth://AA:BB:CC:DD:EE:FF", nullptr);
    const auto second = module.createDevice("bluetooth://11:22:33:44:55:66", nullptr);

    EXPECT_NE(first.getLocalId(), second.getLocalId());
    EXPECT_EQ(serviceProperties(first).getCount(), 2u);
    EXPECT_EQ(serviceProperties(second).getCount(), 2u);

    // Each device owns its own service objects, so two peripherals of the same model cannot share
    // or overwrite one another's values however identical their UUIDs are.
    EXPECT_EQ(static_cast<Int>(first.getPropertyValue(BatteryLevel)), 0x5A);
    EXPECT_EQ(static_cast<Int>(second.getPropertyValue(BatteryLevel)), 0x5A);
    EXPECT_NE(first.getPropertyValue(BatteryService), second.getPropertyValue(BatteryService));
}

TEST(BluetoothDevice, ReflectsAnUnsolicitedDisconnect)
{
    // AutoReconnect off, so the drop is the end of the story: with it on the device would be back
    // before the assertion ran, which is what the reconnect tests cover instead. The state is now
    // published from the reconnector's own thread rather than the backend's, so it is waited on:
    // a backend thread holds the connection's bus mutex, and taking openDAQ's config lock there
    // would deadlock against a caller writing a characteristic.
    auto config = instantConfig();
    config.setPropertyValue("AutoReconnect", False);

    auto owned = std::make_unique<FakeCentral>();
    auto* central = owned.get();
    central->addPeripheral("AA:BB:CC:DD:EE:FF", peripheralUnderTest());

    const auto module =
        createWithImplementation<IModule, BluetoothModule>(NullContext(), std::move(owned), config);
    const auto device = module.createDevice("bluetooth://AA:BB:CC:DD:EE:FF", nullptr, config);

    ASSERT_TRUE(static_cast<Bool>(device.getPropertyValue("Connected")));

    central->connection()->simulateDisconnect();

    EXPECT_TRUE(waitFor([&] { return !static_cast<Bool>(device.getPropertyValue("Connected")); }));
}

TEST(BluetoothDevice, ReleasesThePeripheralWhenTheDeviceIsDestroyed)
{
    auto owned = std::make_unique<FakeCentral>();
    auto* central = owned.get();
    central->addPeripheral("AA:BB:CC:DD:EE:FF", peripheralUnderTest());

    std::shared_ptr<bool> destroyed;
    {
        const auto module = createWithImplementation<IModule, BluetoothModule>(
            NullContext(), std::move(owned), instantConfig());
        const auto device = module.createDevice("bluetooth://AA:BB:CC:DD:EE:FF", nullptr);

        device.getPropertyValue(BatteryLevel);

        // The module owns the backend, so the flag has to be taken while the module is alive.
        destroyed = central->connectionDestroyedFlag();
        ASSERT_TRUE(destroyed);
        EXPECT_FALSE(*destroyed);
    }

    EXPECT_TRUE(*destroyed)
        << "The device did not release its BLE connection; something still holds a reference to it.";
}

TEST(BluetoothDevice, LeavesNoDeviceBehind)
{
    // Every characteristic property carries handlers stored on the very device they act on, and a
    // read additionally queues a job on the device's own refresh worker. All of them capture a raw
    // `this` for that reason: capturing a DevicePtr would make the device hold a reference to
    // itself, a refcount cycle no destructor can break -- so observe it through a weak reference.
    WeakRefPtr<IDevice> weakDevice;

    {
        auto owned = std::make_unique<FakeCentral>();
        owned->addPeripheral("AA:BB:CC:DD:EE:FF", peripheralUnderTest());

        const auto module = createWithImplementation<IModule, BluetoothModule>(
            NullContext(), std::move(owned), instantConfig());
        const auto device = module.createDevice("bluetooth://AA:BB:CC:DD:EE:FF", nullptr);

        device.getPropertyValue(BatteryLevel);
        device.setPropertyValue(UartRx, "01");

        weakDevice = WeakRefPtr<IDevice>(device);
        ASSERT_TRUE(weakDevice.getRef().assigned());
    }

    // Waited on rather than asserted outright: a refresh in flight holds a reference to the
    // device for as long as it runs, which is exactly what stops it being destroyed underneath
    // one. A cycle would never release it, however long the wait.
    EXPECT_TRUE(waitFor([&] { return !weakDevice.getRef().assigned(); }))
        << "A device outlived its owner -- most likely a property value holding a reference back "
           "to the object that owns it.";
}

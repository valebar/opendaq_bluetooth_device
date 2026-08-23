#include "fake_ble_central.h"

#include <bluetooth_module/bluetooth_module.h>
#include <bluetooth_module/connection_string.h>

#include <gtest/gtest.h>

#include <opendaq/context_factory.h>

#include <algorithm>

using namespace daq;
using namespace daq::modules::bluetooth_module;
namespace ble = opendaq_bt::ble;

namespace
{
/*! Module defaults wait 2s for the radio to warm up; a scripted backend needs none of that. */
PropertyObjectPtr instantConfig(bool filterByKnownProfiles = true)
{
    auto config = BluetoothModule::createDefaultConfig();
    config.setPropertyValue("ScanWarmupMs", 0);
    config.setPropertyValue("FilterByKnownProfiles", filterByKnownProfiles);
    return config;
}

/*!
 * A context carrying module options, as a config provider would populate them.
 *
 * The "Modules" -> module id nesting is required: getModuleOptions() looks the module up there, and
 * a flat dictionary resolves to no options at all, silently, so a test built on one would pass for
 * the wrong reason.
 */
ContextPtr contextWithModuleOptions(const DictPtr<IString, IBaseObject>& moduleOptions)
{
    auto modules = Dict<IString, IBaseObject>();
    modules.set("BluetoothModule", moduleOptions);

    auto options = Dict<IString, IBaseObject>();
    options.set("Modules", modules);

    return NullContext(Logger(), TypeManager(), options);
}
}

using opendaq_bt::testing::FakeCentral;

namespace
{

/*!
 * An advertisement the module has a profile for: the Heart Rate service (0x180D).
 *
 * Discovery filters on this UUID by default, so an advertisement built with another service would
 * not be reported.
 */
ble::Advertisement advertisement(const std::string& id, const std::string& name, int16_t rssi)
{
    ble::Advertisement result;
    result.id = id;
    result.name = name;
    result.rssi = rssi;
    result.lastSeen = std::chrono::steady_clock::now();
    result.serviceUuids = {"0000180d-0000-1000-8000-00805f9b34fb"};
    return result;
}

/*!
 * Same, but advertising a service no profile claims. Battery Service is the honest example: it is
 * perfectly browsable over GATT, it just has no decoder here, which is exactly the case the
 * discovery filter is meant to hide.
 */
ble::Advertisement unknownAdvertisement(const std::string& id, const std::string& name, int16_t rssi)
{
    auto result = advertisement(id, name, rssi);
    result.serviceUuids = {"0000180f-0000-1000-8000-00805f9b34fb"};
    return result;
}

/*!
 * A peripheral this host already holds a link to, shaped like the real thing: no RSSI reading, a
 * sighting far outside any device timeout because it stopped advertising the moment it connected,
 * and the *resolved* GATT service list rather than whatever fitted in an advertisement.
 */
ble::Advertisement connectedAdvertisement(const std::string& id, const std::string& name)
{
    ble::Advertisement result;
    result.id = id;
    result.name = name;
    result.rssi = 0;
    result.connected = true;
    result.lastSeen = std::chrono::steady_clock::now() - std::chrono::minutes(5);
    result.serviceUuids = {"00001800-0000-1000-8000-00805f9b34fb",
                           "0000180a-0000-1000-8000-00805f9b34fb",
                           "0000180d-0000-1000-8000-00805f9b34fb",
                           "0000180f-0000-1000-8000-00805f9b34fb"};
    return result;
}

FakeCentral::Peripheral batteryPeripheral()
{
    ble::Characteristic level;
    level.uuid = "00002a19-0000-1000-8000-00805f9b34fb";
    level.handle = "/fake/service0/char0";
    level.properties = ble::CharPropRead | ble::CharPropNotify;

    ble::Characteristic control;
    control.uuid = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
    control.handle = "/fake/service0/char1";
    control.properties = ble::CharPropWrite;

    ble::Service service;
    service.uuid = "0000180f-0000-1000-8000-00805f9b34fb";
    service.primary = true;
    service.characteristics = {level, control};

    FakeCentral::Peripheral peripheral;
    peripheral.services = {service};
    peripheral.values["/fake/service0/char0"] = {0x5A};

    return peripheral;
}

/*! Builds a module over a scripted backend, handing back a raw pointer for test scripting. */
ModulePtr createModule(FakeCentral*& centralOut, std::unique_ptr<FakeCentral> central = nullptr)
{
    if (!central)
        central = std::make_unique<FakeCentral>();

    centralOut = central.get();

    return createWithImplementation<IModule, BluetoothModule>(NullContext(), std::move(central), instantConfig());
}

/*! As createModule, but with explicit control of the module config and the context. */
ModulePtr createConfiguredModule(FakeCentral*& centralOut,
                                 const PropertyObjectPtr& config,
                                 const ContextPtr& context = nullptr)
{
    auto central = std::make_unique<FakeCentral>();
    centralOut = central.get();

    return createWithImplementation<IModule, BluetoothModule>(
        context.assigned() ? context : NullContext(), std::move(central), config);
}

}

TEST(BluetoothModule, AdvertisesItsDeviceTypeWithTheBluetoothPrefix)
{
    FakeCentral* central = nullptr;
    const auto module = createModule(central);

    const auto types = module.getAvailableDeviceTypes();
    ASSERT_EQ(types.getCount(), 1u);

    const DeviceTypePtr type = types.get("bluetooth_ble");
    ASSERT_TRUE(type.assigned());

    // This prefix is the whole reason "bluetooth://..." reaches this module.
    EXPECT_EQ(type.getConnectionStringPrefix(), "bluetooth");
}

TEST(BluetoothModule, ReportsDiscoveredPeripheralsWithConnectionStrings)
{
    FakeCentral* central = nullptr;
    const auto module = createModule(central);

    central->addAdvertisement(advertisement("AA:BB:CC:DD:EE:FF", "Sensor One", -42));
    central->addAdvertisement(advertisement("11:22:33:44:55:66", "Sensor Two", -80));

    const auto devices = module.getAvailableDevices();
    ASSERT_EQ(devices.getCount(), 2u);

    // The backend's order, passed through untouched -- the weaker signal stays second only because
    // it was seen second, not because anything sorted on RSSI.
    EXPECT_EQ(devices[0].getConnectionString(), "bluetooth://AA:BB:CC:DD:EE:FF");
    EXPECT_EQ(devices[0].getName(), "Sensor One");
    EXPECT_EQ(devices[1].getConnectionString(), "bluetooth://11:22:33:44:55:66");
}

TEST(BluetoothModule, DiscoveryCarriesRssiAndAddressForPickingADevice)
{
    FakeCentral* central = nullptr;
    const auto module = createModule(central);

    central->addAdvertisement(advertisement("AA:BB:CC:DD:EE:FF", "Sensor One", -42));

    const auto devices = module.getAvailableDevices();
    ASSERT_EQ(devices.getCount(), 1u);

    EXPECT_EQ(static_cast<Int>(devices[0].getPropertyValue("Rssi")), -42);
    EXPECT_EQ(devices[0].getPropertyValue("Address"), "AA:BB:CC:DD:EE:FF");
}

TEST(BluetoothModule, StartsScanningOnceAcrossRepeatedDiscoveryCalls)
{
    FakeCentral* central = nullptr;
    const auto module = createModule(central);

    module.getAvailableDevices();
    module.getAvailableDevices();
    module.getAvailableDevices();

    EXPECT_EQ(central->scans(), 1);
    EXPECT_EQ(central->discoveries(), 3);
}

TEST(BluetoothModule, DropsPeripheralsThatHaveGoneStale)
{
    FakeCentral* central = nullptr;
    const auto module = createModule(central);

    auto stale = advertisement("AA:BB:CC:DD:EE:FF", "Long Gone", -42);
    stale.lastSeen = std::chrono::steady_clock::now() - std::chrono::minutes(5);
    central->addAdvertisement(stale);
    central->addAdvertisement(advertisement("11:22:33:44:55:66", "Still Here", -60));

    const auto devices = module.getAvailableDevices();
    ASSERT_EQ(devices.getCount(), 1u);
    EXPECT_EQ(devices[0].getName(), "Still Here");
}

TEST(BluetoothModule, ListsAPeripheralConnectedToThisHostThoughItIsNotAdvertising)
{
    FakeCentral* central = nullptr;
    const auto module = createModule(central);

    // The bug this guards: a peripheral connected to the host stops advertising, so it produces no
    // sighting and its last one recedes past DeviceTimeoutMs. Connecting to a heart rate strap once
    // used to hide it from every later discovery, which is the one device the module can use.
    central->addAdvertisement(connectedAdvertisement("02:00:00:00:00:01", "Chest Strap"));

    const auto devices = module.getAvailableDevices();
    ASSERT_EQ(devices.getCount(), 1u);
    EXPECT_EQ(devices[0].getName(), "Chest Strap");
}

TEST(BluetoothModule, MarksAConnectedPeripheralInItsDeviceInfo)
{
    FakeCentral* central = nullptr;
    const auto module = createModule(central);

    central->addAdvertisement(connectedAdvertisement("02:00:00:00:00:01", "Chest Strap"));
    central->addAdvertisement(advertisement("AA:BB:CC:DD:EE:FF", "Still Advertising", -42));

    const auto devices = module.getAvailableDevices();
    ASSERT_EQ(devices.getCount(), 2u);

    EXPECT_TRUE(static_cast<bool>(devices[0].getPropertyValue("Connected")));
    EXPECT_FALSE(static_cast<bool>(devices[1].getPropertyValue("Connected")));

    // Rssi stays in the schema whatever the link state; 0 there means "no reading", and Connected
    // is what a client uses to tell that apart from a very strong signal.
    EXPECT_EQ(static_cast<Int>(devices[0].getPropertyValue("Rssi")), 0);
    EXPECT_EQ(static_cast<Int>(devices[1].getPropertyValue("Rssi")), -42);
}

TEST(BluetoothModule, StillHidesAConnectedPeripheralWithNoProfileTheModuleUnderstands)
{
    FakeCentral* central = nullptr;
    const auto module = createModule(central);

    // Connected keyboards and mice reach the filter now that being connected is enough to be
    // reported. They are hidden on the same rule as anything else: no profile, no listing.
    auto keyboard = connectedAdvertisement("02:00:00:00:00:02", "A Keyboard");
    keyboard.serviceUuids = {"00001800-0000-1000-8000-00805f9b34fb",
                             "00001812-0000-1000-8000-00805f9b34fb"};
    central->addAdvertisement(keyboard);

    EXPECT_EQ(module.getAvailableDevices().getCount(), 0u);
}

TEST(BluetoothModule, ListsAConnectedPeripheralWithNoKnownProfileWhenTheFilterIsOff)
{
    FakeCentral* central = nullptr;
    const auto module = createConfiguredModule(central, instantConfig(false));

    auto keyboard = connectedAdvertisement("02:00:00:00:00:02", "A Keyboard");
    keyboard.serviceUuids = {"00001812-0000-1000-8000-00805f9b34fb"};
    central->addAdvertisement(keyboard);

    EXPECT_EQ(module.getAvailableDevices().getCount(), 1u);
}

TEST(BluetoothModule, TellsTheBackendWhichServicesItHasProfilesFor)
{
    FakeCentral* central = nullptr;
    const auto module = createModule(central);

    // CoreBluetooth cannot enumerate connected peripherals without being told what to look for,
    // and the profile registry is the only thing that knows.
    const auto services = central->interestingServices();
    ASSERT_FALSE(services.empty());

    // Every profile in the registry, so adding one widens what a Mac can find as well as what a
    // connected device streams.
    for (const auto* uuid : {"0000180d-0000-1000-8000-00805f9b34fb",    // Heart Rate
                             "00001818-0000-1000-8000-00805f9b34fb",    // Cycling Power
                             "00001816-0000-1000-8000-00805f9b34fb",    // Cycling Speed and Cadence
                             "00001826-0000-1000-8000-00805f9b34fb"})   // Fitness Machine
        EXPECT_NE(std::find(services.begin(), services.end(), uuid), services.end()) << uuid;
}

TEST(BluetoothModule, HidesPeripheralsWithNoProfileTheModuleUnderstands)
{
    FakeCentral* central = nullptr;
    const auto module = createModule(central);

    central->addAdvertisement(advertisement("AA:BB:CC:DD:EE:FF", "Chest Strap", -42));
    central->addAdvertisement(unknownAdvertisement("11:22:33:44:55:66", "Someone's Earbuds", -50));

    const auto devices = module.getAvailableDevices();
    ASSERT_EQ(devices.getCount(), 1u);
    EXPECT_EQ(devices[0].getName(), "Chest Strap");
}

TEST(BluetoothModule, HidesPeripheralsThatAdvertiseNoServicesAtAll)
{
    FakeCentral* central = nullptr;
    const auto module = createModule(central);

    // Strict by decision, not by accident: a peripheral revealing its services only once connected
    // is hidden rather than shown on the chance that it turns out to be usable.
    auto silent = advertisement("AA:BB:CC:DD:EE:FF", "Says Nothing", -42);
    silent.serviceUuids.clear();
    central->addAdvertisement(silent);

    EXPECT_EQ(module.getAvailableDevices().getCount(), 0u);
}

TEST(BluetoothModule, MatchesProfilesAdvertisedAsSixteenBitShorthand)
{
    FakeCentral* central = nullptr;
    const auto module = createModule(central);

    // BlueZ copies its UUIDs list through without expanding it, so the filter has to compare in
    // expanded form rather than by string equality.
    auto shorthand = advertisement("AA:BB:CC:DD:EE:FF", "Chest Strap", -42);
    shorthand.serviceUuids = {"180D"};
    central->addAdvertisement(shorthand);

    EXPECT_EQ(module.getAvailableDevices().getCount(), 1u);
}

TEST(BluetoothModule, ReportsEveryPeripheralWhenTheProfileFilterIsOff)
{
    FakeCentral* central = nullptr;
    const auto module = createConfiguredModule(central, instantConfig(false));

    central->addAdvertisement(advertisement("AA:BB:CC:DD:EE:FF", "Chest Strap", -42));
    central->addAdvertisement(unknownAdvertisement("11:22:33:44:55:66", "Someone's Earbuds", -50));

    EXPECT_EQ(module.getAvailableDevices().getCount(), 2u);
}

TEST(BluetoothModule, ModuleOptionsCanTurnTheProfileFilterOff)
{
    auto options = Dict<IString, IBaseObject>();
    options.set("FilterByKnownProfiles", Boolean(false));

    FakeCentral* central = nullptr;
    const auto module =
        createConfiguredModule(central, instantConfig(), contextWithModuleOptions(options));

    central->addAdvertisement(advertisement("AA:BB:CC:DD:EE:FF", "Chest Strap", -42));
    central->addAdvertisement(unknownAdvertisement("11:22:33:44:55:66", "Someone's Earbuds", -50));

    EXPECT_EQ(module.getAvailableDevices().getCount(), 2u);
}

TEST(BluetoothModule, ModuleOptionsOverrideTheConfigObjectForTheProfileFilter)
{
    auto options = Dict<IString, IBaseObject>();
    options.set("FilterByKnownProfiles", Boolean(false));

    // Module options deliberately beat the constructor config. The ModuleManager can only build the
    // module without one, so in production the config is always createDefaultConfig() -- if it won,
    // the module option would never be able to change anything.
    FakeCentral* central = nullptr;
    const auto module =
        createConfiguredModule(central, instantConfig(true), contextWithModuleOptions(options));

    central->addAdvertisement(advertisement("AA:BB:CC:DD:EE:FF", "Chest Strap", -42));
    central->addAdvertisement(unknownAdvertisement("11:22:33:44:55:66", "Someone's Earbuds", -50));

    EXPECT_EQ(module.getAvailableDevices().getCount(), 2u);
}

TEST(BluetoothModule, KeepsTheAdvertisedNameOfAPeripheralDiscoveryFilteredOut)
{
    FakeCentral* central = nullptr;
    const auto module = createModule(central);

    central->addAdvertisement(unknownAdvertisement("AA:BB:CC:DD:EE:FF", "Hidden Sensor", -50));
    central->addPeripheral("AA:BB:CC:DD:EE:FF", batteryPeripheral());

    ASSERT_EQ(module.getAvailableDevices().getCount(), 0u);

    // The filter narrows the list, nothing else. Were it applied inside ScanCache, onCreateDevice's
    // name lookup would come back empty and the device would fall back to "Bluetooth device AA:...".
    const auto device = module.createDevice("bluetooth://AA:BB:CC:DD:EE:FF", nullptr);
    EXPECT_EQ(device.getName(), "Hidden Sensor");
}

TEST(BluetoothModule, ReportsNoDevicesWhenBluetoothIsUnavailable)
{
    auto central = std::make_unique<FakeCentral>();
    central->setUnavailable("the Bluetooth adapter is powered off");

    FakeCentral* raw = nullptr;
    const auto module = createModule(raw, std::move(central));

    // An adapterless or switched-off host is a normal condition, not an error: discovery has to
    // keep working for every other module in the instance.
    EXPECT_EQ(module.getAvailableDevices().getCount(), 0u);
    EXPECT_EQ(raw->scans(), 0);
}

TEST(BluetoothModule, RejectsConnectionStringsBelongingToOtherModules)
{
    FakeCentral* central = nullptr;
    const auto module = createModule(central);

    ASSERT_THROW(module.createDevice("daq.opcua://192.168.0.10", nullptr), InvalidParameterException);
    EXPECT_TRUE(central->connectedId().empty());
}

TEST(BluetoothModule, RejectsMalformedBluetoothIdentifiers)
{
    FakeCentral* central = nullptr;
    const auto module = createModule(central);

    ASSERT_THROW(module.createDevice("bluetooth://not-a-device", nullptr), InvalidParameterException);
}

TEST(BluetoothModule, ConnectsUsingTheNormalisedIdentifier)
{
    FakeCentral* central = nullptr;
    const auto module = createModule(central);

    central->addPeripheral("AA:BB:CC:DD:EE:FF", batteryPeripheral());

    // Lower case going in; the backend must be asked for the canonical form.
    const auto device = module.createDevice("bluetooth://aa:bb:cc:dd:ee:ff", nullptr);

    ASSERT_TRUE(device.assigned());
    EXPECT_EQ(central->connectedId(), "AA:BB:CC:DD:EE:FF");
}

TEST(BluetoothModule, ReportsUnreachablePeripherals)
{
    FakeCentral* central = nullptr;
    const auto module = createModule(central);

    ASSERT_THROW(module.createDevice("bluetooth://AA:BB:CC:DD:EE:FF", nullptr), std::exception);
}

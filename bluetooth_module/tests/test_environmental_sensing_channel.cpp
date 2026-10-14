#include "fake_ble_central.h"

#include <bluetooth_module/bluetooth_module.h>
#include <bluetooth_module/profiles/environmental_sensing_channel.h>

#include <gtest/gtest.h>

#include <opendaq/context_factory.h>
#include <opendaq/reader_factory.h>
#include <opendaq/search_filter_factory.h>

#include <initializer_list>

using namespace daq;
using namespace daq::modules::bluetooth_module;
using opendaq_bt::testing::FakeCentral;

namespace ble = opendaq_bt::ble;

namespace
{

constexpr char IndoorHandle[] = "/fake/ess/temperature1";
constexpr char OutdoorHandle[] = "/fake/ess/temperature2";

PropertyObjectPtr instantConfig()
{
    auto config = BluetoothModule::createDefaultConfig();
    config.setPropertyValue("ScanWarmupMs", 0);
    return config;
}

ble::Characteristic temperatureCharacteristic(const char* handle, uint32_t properties)
{
    ble::Characteristic characteristic;
    characteristic.uuid = EnvironmentalSensingChannel::temperatureCharacteristicUuid();
    characteristic.handle = handle;
    characteristic.properties = properties;
    return characteristic;
}

/*! An ESS peripheral with one Temperature characteristic per handle, readable and notifying. */
FakeCentral::Peripheral sensorPeripheral(std::initializer_list<const char*> handles = {IndoorHandle})
{
    ble::Service sensing;
    sensing.uuid = EnvironmentalSensingChannel::serviceUuid();
    sensing.primary = true;

    FakeCentral::Peripheral peripheral;
    for (const auto* handle : handles)
    {
        sensing.characteristics.push_back(
            temperatureCharacteristic(handle, ble::CharPropRead | ble::CharPropNotify));
        peripheral.values[handle] = {0x0A, 0x0A};
    }

    peripheral.services = {sensing};
    return peripheral;
}

struct Connected
{
    ModulePtr module;
    DevicePtr device;
    FakeCentral* central = nullptr;
};

Connected connect(FakeCentral::Peripheral peripheral, std::initializer_list<const char*> refused = {})
{
    auto owned = std::make_unique<FakeCentral>();
    auto* central = owned.get();
    central->addPeripheral("AA:BB:CC:DD:EE:FF", std::move(peripheral));
    for (const auto* handle : refused)
        central->refuseSubscriptionsFor(handle);

    const auto module =
        createWithImplementation<IModule, BluetoothModule>(NullContext(), std::move(owned), instantConfig());

    return {module, module.createDevice("bluetooth://AA:BB:CC:DD:EE:FF", nullptr), central};
}

ChannelPtr sensingChannel(const DevicePtr& device)
{
    for (const auto& channel : device.getChannels())
    {
        if (channel.getLocalId() == "EnvironmentalSensing")
            return channel;
    }
    return nullptr;
}

SignalPtr signalByLocalId(const ChannelPtr& channel, const std::string& localId)
{
    for (const auto& signal : channel.getSignals(search::Any()))
    {
        if (signal.getLocalId() == localId)
            return signal;
    }
    return nullptr;
}

/*! Skips the descriptor-changed event a fresh signal emits first; see test_heart_rate_channel. */
StreamReaderPtr dataReader(const SignalPtr& signal)
{
    return StreamReaderBuilder()
        .setSignal(signal)
        .setValueReadType(SampleType::Float64)
        .setDomainReadType(SampleType::Int64)
        .setSkipEvents(True)
        .build();
}

std::vector<double> readAll(StreamReaderPtr& reader)
{
    double values[16] = {};
    SizeT count = 16;
    reader.read(values, &count);
    return {values, values + count};
}

}

TEST(EnvironmentalSensingChannel, AppearsWithACelsiusTemperatureSignal)
{
    const auto connected = connect(sensorPeripheral());
    const auto channel = sensingChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    const auto signal = signalByLocalId(channel, "Temperature");
    ASSERT_TRUE(signal.assigned());
    EXPECT_EQ(signal.getDescriptor().getUnit().getSymbol(), "\xC2\xB0" "C");
    EXPECT_EQ(signal.getDomainSignal().getDescriptor().getRule().getType(), DataRuleType::Explicit);
    EXPECT_TRUE(connected.central->connection()->isSubscribed(IndoorHandle));
}

TEST(EnvironmentalSensingChannel, EmitsASamplePerNotification)
{
    auto connected = connect(sensorPeripheral());
    const auto channel = sensingChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto reader = dataReader(signalByLocalId(channel, "Temperature"));

    connected.central->connection()->pushNotification(IndoorHandle, {0x0A, 0x0A});   // 25.70
    connected.central->connection()->pushNotification(IndoorHandle, {0xE6, 0xFB});   // -10.50

    const auto values = readAll(reader);
    ASSERT_EQ(values.size(), 2u);
    EXPECT_DOUBLE_EQ(values[0], 25.70);
    EXPECT_DOUBLE_EQ(values[1], -10.5);
}

TEST(EnvironmentalSensingChannel, GivesEachTemperatureInstanceItsOwnSignal)
{
    auto connected = connect(sensorPeripheral({IndoorHandle, OutdoorHandle}));
    const auto channel = sensingChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto indoor = dataReader(signalByLocalId(channel, "Temperature"));
    auto outdoor = dataReader(signalByLocalId(channel, "Temperature2"));

    connected.central->connection()->pushNotification(OutdoorHandle, {0xE6, 0xFB});

    EXPECT_TRUE(readAll(indoor).empty());
    const auto values = readAll(outdoor);
    ASSERT_EQ(values.size(), 1u);
    EXPECT_DOUBLE_EQ(values[0], -10.5);
}

TEST(EnvironmentalSensingChannel, EmitsNothingForTheUnknownSentinel)
{
    auto connected = connect(sensorPeripheral());
    const auto channel = sensingChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto reader = dataReader(signalByLocalId(channel, "Temperature"));

    connected.central->connection()->pushNotification(IndoorHandle, {0x00, 0x80});
    ASSERT_NO_THROW(connected.central->connection()->pushNotification(IndoorHandle, {0x0A}));

    EXPECT_TRUE(readAll(reader).empty());
}

TEST(EnvironmentalSensingChannel, SkipsTemperaturesThatOnlyRead)
{
    auto peripheral = sensorPeripheral({IndoorHandle, OutdoorHandle});
    peripheral.services[0].characteristics[0].properties = ble::CharPropRead;

    const auto connected = connect(std::move(peripheral));
    const auto channel = sensingChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    // The notifying one becomes the first signal; nothing is reserved for the skipped one.
    EXPECT_TRUE(signalByLocalId(channel, "Temperature").assigned());
    EXPECT_FALSE(signalByLocalId(channel, "Temperature2").assigned());
    EXPECT_TRUE(connected.central->connection()->isSubscribed(OutdoorHandle));
}

TEST(EnvironmentalSensingChannel, IsSkippedWhenNoTemperatureNotifies)
{
    auto peripheral = sensorPeripheral();
    peripheral.services[0].characteristics[0].properties = ble::CharPropRead;

    const auto connected = connect(std::move(peripheral));

    EXPECT_EQ(connected.device.getChannels().getCount(), 0u);
    EXPECT_TRUE(connected.device.hasProperty("Environmental Sensing"));
}

TEST(EnvironmentalSensingChannel, KeepsTheTemperaturesThatSubscribed)
{
    auto connected = connect(sensorPeripheral({IndoorHandle, OutdoorHandle}), {IndoorHandle});
    const auto channel = sensingChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    EXPECT_FALSE(connected.central->connection()->isSubscribed(IndoorHandle));
    EXPECT_TRUE(connected.central->connection()->isSubscribed(OutdoorHandle));

    auto reader = dataReader(signalByLocalId(channel, "Temperature2"));
    connected.central->connection()->pushNotification(OutdoorHandle, {0x0A, 0x0A});
    EXPECT_EQ(readAll(reader).size(), 1u);
}

TEST(EnvironmentalSensingChannel, IsSkippedWhenEverySubscriptionIsRefused)
{
    const auto connected =
        connect(sensorPeripheral({IndoorHandle, OutdoorHandle}), {IndoorHandle, OutdoorHandle});

    EXPECT_EQ(connected.device.getChannels().getCount(), 0u);
}

TEST(EnvironmentalSensingChannel, ReleasesTheConnectionWhenTheDeviceGoesAway)
{
    auto owned = std::make_unique<FakeCentral>();
    auto* central = owned.get();
    central->addPeripheral("AA:BB:CC:DD:EE:FF", sensorPeripheral({IndoorHandle, OutdoorHandle}));

    std::shared_ptr<bool> destroyed;
    {
        const auto module = createWithImplementation<IModule, BluetoothModule>(
            NullContext(), std::move(owned), instantConfig());
        const auto device = module.createDevice("bluetooth://AA:BB:CC:DD:EE:FF", nullptr);

        ASSERT_TRUE(central->connection()->isSubscribed(OutdoorHandle));
        destroyed = central->connectionDestroyedFlag();
    }

    EXPECT_TRUE(*destroyed);
}

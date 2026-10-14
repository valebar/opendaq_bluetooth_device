#include "fake_ble_central.h"

#include <bluetooth_module/bluetooth_module.h>
#include <bluetooth_module/profiles/health_thermometer_channel.h>

#include <gtest/gtest.h>

#include <opendaq/context_factory.h>
#include <opendaq/reader_factory.h>
#include <opendaq/search_filter_factory.h>

using namespace daq;
using namespace daq::modules::bluetooth_module;
using opendaq_bt::testing::FakeCentral;

namespace ble = opendaq_bt::ble;

namespace
{

constexpr char MeasurementHandle[] = "/fake/ht/measurement";
constexpr char TypeHandle[] = "/fake/ht/type";
constexpr char IntermediateHandle[] = "/fake/ht/intermediate";

PropertyObjectPtr instantConfig()
{
    auto config = BluetoothModule::createDefaultConfig();
    config.setPropertyValue("ScanWarmupMs", 0);
    return config;
}

/*! A thermometer with an indicated measurement, a readable type and an intermediate stream. */
FakeCentral::Peripheral thermometerPeripheral(bool withIntermediate = true)
{
    ble::Characteristic measurement;
    measurement.uuid = HealthThermometerChannel::measurementCharacteristicUuid();
    measurement.handle = MeasurementHandle;
    measurement.properties = ble::CharPropIndicate;

    ble::Characteristic type;
    type.uuid = HealthThermometerChannel::temperatureTypeCharacteristicUuid();
    type.handle = TypeHandle;
    type.properties = ble::CharPropRead;

    ble::Service thermometer;
    thermometer.uuid = HealthThermometerChannel::serviceUuid();
    thermometer.primary = true;
    thermometer.characteristics = {measurement, type};

    if (withIntermediate)
    {
        ble::Characteristic intermediate;
        intermediate.uuid = HealthThermometerChannel::intermediateTemperatureCharacteristicUuid();
        intermediate.handle = IntermediateHandle;
        intermediate.properties = ble::CharPropNotify;
        thermometer.characteristics.push_back(intermediate);
    }

    FakeCentral::Peripheral peripheral;
    peripheral.services = {thermometer};
    peripheral.values[TypeHandle] = {0x02};   // Body

    return peripheral;
}

struct Connected
{
    ModulePtr module;
    DevicePtr device;
    FakeCentral* central = nullptr;
};

Connected connect(FakeCentral::Peripheral peripheral, const char* refusedHandle = nullptr)
{
    auto owned = std::make_unique<FakeCentral>();
    auto* central = owned.get();
    central->addPeripheral("AA:BB:CC:DD:EE:FF", std::move(peripheral));
    if (refusedHandle != nullptr)
        central->refuseSubscriptionsFor(refusedHandle);

    const auto module =
        createWithImplementation<IModule, BluetoothModule>(NullContext(), std::move(owned), instantConfig());

    return {module, module.createDevice("bluetooth://AA:BB:CC:DD:EE:FF", nullptr), central};
}

ChannelPtr thermometerChannel(const DevicePtr& device)
{
    for (const auto& channel : device.getChannels())
    {
        if (channel.getLocalId() == "HealthThermometer")
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

constexpr uint8_t Fahrenheit = 0x01;
constexpr uint8_t TypePresent = 0x04;

/*! 36.5 degC: mantissa 365, exponent -1. */
const std::vector<uint8_t> ThirtySixFive = {0x00, 0x6D, 0x01, 0x00, 0xFF};

}

TEST(HealthThermometerChannel, AppearsAsAChannelOnAPeripheralThatSupportsIt)
{
    const auto connected = connect(thermometerPeripheral());

    EXPECT_TRUE(thermometerChannel(connected.device).assigned());
}

TEST(HealthThermometerChannel, SubscribesToMeasurementAndIntermediate)
{
    const auto connected = connect(thermometerPeripheral());

    EXPECT_TRUE(connected.central->connection()->isSubscribed(MeasurementHandle));
    EXPECT_TRUE(connected.central->connection()->isSubscribed(IntermediateHandle));
}

TEST(HealthThermometerChannel, PublishesCelsiusSignals)
{
    const auto connected = connect(thermometerPeripheral());
    const auto channel = thermometerChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    for (const auto* id : {"Temperature", "IntermediateTemperature"})
    {
        const auto signal = signalByLocalId(channel, id);
        ASSERT_TRUE(signal.assigned()) << id;
        EXPECT_EQ(signal.getDescriptor().getUnit().getSymbol(), "\xC2\xB0" "C") << id;
        EXPECT_EQ(signal.getDomainSignal().getDescriptor().getRule().getType(), DataRuleType::Explicit);
    }
}

TEST(HealthThermometerChannel, EmitsASamplePerIndication)
{
    auto connected = connect(thermometerPeripheral());
    const auto channel = thermometerChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto reader = dataReader(signalByLocalId(channel, "Temperature"));

    connected.central->connection()->pushNotification(MeasurementHandle, ThirtySixFive);
    connected.central->connection()->pushNotification(MeasurementHandle,
                                                      {Fahrenheit, 0xDA, 0x03, 0x00, 0xFF});   // 98.6 degF

    const auto values = readAll(reader);
    ASSERT_EQ(values.size(), 2u);
    EXPECT_DOUBLE_EQ(values[0], 36.5);
    EXPECT_NEAR(values[1], 37.0, 1e-9);
}

TEST(HealthThermometerChannel, KeepsIntermediateReadingsOffTheTemperatureSignal)
{
    auto connected = connect(thermometerPeripheral());
    const auto channel = thermometerChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto finalReader = dataReader(signalByLocalId(channel, "Temperature"));
    auto intermediate = dataReader(signalByLocalId(channel, "IntermediateTemperature"));

    connected.central->connection()->pushNotification(IntermediateHandle, ThirtySixFive);

    EXPECT_TRUE(readAll(finalReader).empty());
    const auto values = readAll(intermediate);
    ASSERT_EQ(values.size(), 1u);
    EXPECT_DOUBLE_EQ(values[0], 36.5);
}

TEST(HealthThermometerChannel, ReadsTheTemperatureTypeOnConnect)
{
    const auto connected = connect(thermometerPeripheral());
    const auto channel = thermometerChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    EXPECT_EQ(channel.getPropertyValue("TemperatureType"), "Body");
}

TEST(HealthThermometerChannel, TakesTheTemperatureTypeFromTheMeasurement)
{
    auto connected = connect(thermometerPeripheral());
    const auto channel = thermometerChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    connected.central->connection()->pushNotification(MeasurementHandle,
                                                      {TypePresent, 0x6D, 0x01, 0x00, 0xFF, 0x09});

    EXPECT_EQ(channel.getPropertyValue("TemperatureType"), "Tympanum");
}

TEST(HealthThermometerChannel, EmitsNothingForANoReadingFrame)
{
    auto connected = connect(thermometerPeripheral());
    const auto channel = thermometerChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto reader = dataReader(signalByLocalId(channel, "Temperature"));

    // NaN mantissa: well-formed, but no reading.
    connected.central->connection()->pushNotification(MeasurementHandle, {0x00, 0xFF, 0xFF, 0x7F, 0x00});

    EXPECT_TRUE(readAll(reader).empty());
}

TEST(HealthThermometerChannel, IgnoresMalformedIndications)
{
    auto connected = connect(thermometerPeripheral());
    const auto channel = thermometerChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto reader = dataReader(signalByLocalId(channel, "Temperature"));

    ASSERT_NO_THROW(connected.central->connection()->pushNotification(MeasurementHandle, {0x00, 0x6D}));
    connected.central->connection()->pushNotification(MeasurementHandle, ThirtySixFive);

    const auto values = readAll(reader);
    ASSERT_EQ(values.size(), 1u);
    EXPECT_DOUBLE_EQ(values[0], 36.5);
}

TEST(HealthThermometerChannel, WorksWithoutIntermediateTemperature)
{
    const auto connected = connect(thermometerPeripheral(false));

    EXPECT_TRUE(thermometerChannel(connected.device).assigned());
    EXPECT_TRUE(connected.central->connection()->isSubscribed(MeasurementHandle));
}

TEST(HealthThermometerChannel, SurvivesARefusedIntermediateSubscription)
{
    auto connected = connect(thermometerPeripheral(), IntermediateHandle);
    const auto channel = thermometerChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto reader = dataReader(signalByLocalId(channel, "Temperature"));
    connected.central->connection()->pushNotification(MeasurementHandle, ThirtySixFive);

    EXPECT_EQ(readAll(reader).size(), 1u);
}

TEST(HealthThermometerChannel, IsSkippedWhenTheMeasurementRefusesSubscription)
{
    const auto connected = connect(thermometerPeripheral(), MeasurementHandle);

    EXPECT_EQ(connected.device.getChannels().getCount(), 0u);
    // Declined before the optional subscription, so nothing is left notifying a dead channel.
    EXPECT_FALSE(connected.central->connection()->isSubscribed(IntermediateHandle));
}

TEST(HealthThermometerChannel, IsSkippedWhenTheMeasurementCannotIndicate)
{
    auto peripheral = thermometerPeripheral();
    peripheral.services[0].characteristics[0].properties = ble::CharPropRead;

    const auto connected = connect(std::move(peripheral));

    EXPECT_EQ(connected.device.getChannels().getCount(), 0u);
    EXPECT_TRUE(connected.device.hasProperty("Health Thermometer"));
}

TEST(HealthThermometerChannel, IsSkippedWhenTheServiceLacksAMeasurement)
{
    auto peripheral = thermometerPeripheral();
    auto& characteristics = peripheral.services[0].characteristics;
    characteristics.erase(characteristics.begin());

    const auto connected = connect(std::move(peripheral));

    EXPECT_EQ(connected.device.getChannels().getCount(), 0u);
}

TEST(HealthThermometerChannel, ReleasesTheConnectionWhenTheDeviceGoesAway)
{
    auto owned = std::make_unique<FakeCentral>();
    auto* central = owned.get();
    central->addPeripheral("AA:BB:CC:DD:EE:FF", thermometerPeripheral());

    std::shared_ptr<bool> destroyed;
    {
        const auto module = createWithImplementation<IModule, BluetoothModule>(
            NullContext(), std::move(owned), instantConfig());
        const auto device = module.createDevice("bluetooth://AA:BB:CC:DD:EE:FF", nullptr);

        ASSERT_TRUE(central->connection()->isSubscribed(MeasurementHandle));
        destroyed = central->connectionDestroyedFlag();
    }

    EXPECT_TRUE(*destroyed);
}

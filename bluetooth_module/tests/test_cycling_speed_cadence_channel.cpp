#include "fake_ble_central.h"

#include <bluetooth_module/bluetooth_module.h>
#include <bluetooth_module/profiles/cycling_speed_cadence_channel.h>

#include <gtest/gtest.h>

#include <opendaq/context_factory.h>
#include <opendaq/reader_factory.h>
#include <opendaq/search_filter_factory.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace daq;
using namespace daq::modules::bluetooth_module;
using opendaq_bt::testing::FakeCentral;

namespace ble = opendaq_bt::ble;

namespace
{

constexpr char MeasurementHandle[] = "/fake/csc/measurement";
constexpr char FeatureHandle[] = "/fake/csc/feature";
constexpr char LocationHandle[] = "/fake/csc/location";

PropertyObjectPtr instantConfig()
{
    auto config = BluetoothModule::createDefaultConfig();
    config.setPropertyValue("ScanWarmupMs", 0);
    return config;
}

/*! A speed and cadence sensor: measurement, feature and sensor location. */
FakeCentral::Peripheral cscPeripheral()
{
    ble::Characteristic measurement;
    measurement.uuid = CyclingSpeedCadenceChannel::measurementCharacteristicUuid();
    measurement.handle = MeasurementHandle;
    measurement.properties = ble::CharPropNotify;

    ble::Characteristic feature;
    feature.uuid = CyclingSpeedCadenceChannel::featureCharacteristicUuid();
    feature.handle = FeatureHandle;
    feature.properties = ble::CharPropRead;

    ble::Characteristic location;
    location.uuid = CyclingSpeedCadenceChannel::sensorLocationCharacteristicUuid();
    location.handle = LocationHandle;
    location.properties = ble::CharPropRead;

    ble::Service csc;
    csc.uuid = CyclingSpeedCadenceChannel::serviceUuid();
    csc.primary = true;
    csc.characteristics = {measurement, feature, location};

    FakeCentral::Peripheral peripheral;
    peripheral.services = {csc};
    peripheral.values[FeatureHandle] = {0x03, 0x00};   // wheel and crank data supported
    peripheral.values[LocationHandle] = {0x0B};        // Chainstay

    return peripheral;
}

struct Connected
{
    ModulePtr module;
    DevicePtr device;
    FakeCentral* central = nullptr;
};

Connected connect(FakeCentral::Peripheral peripheral)
{
    auto owned = std::make_unique<FakeCentral>();
    auto* central = owned.get();
    central->addPeripheral("AA:BB:CC:DD:EE:FF", std::move(peripheral));

    const auto module =
        createWithImplementation<IModule, BluetoothModule>(NullContext(), std::move(owned), instantConfig());

    return {module, module.createDevice("bluetooth://AA:BB:CC:DD:EE:FF", nullptr), central};
}

ChannelPtr cscChannel(const DevicePtr& device)
{
    for (const auto& channel : device.getChannels())
    {
        if (channel.getLocalId() == "CyclingSpeedCadence")
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

/*! Skipping events keeps the tests about the data; see the note in test_heart_rate_channel.cpp. */
StreamReaderPtr dataReader(const SignalPtr& signal)
{
    return StreamReaderBuilder()
        .setSignal(signal)
        .setValueReadType(SampleType::Float64)
        .setDomainReadType(SampleType::Int64)
        .setSkipEvents(True)
        .build();
}

/*! A CSC Measurement carrying wheel data alone. Event time is in 1/1024 s ticks. */
std::vector<uint8_t> wheelFrame(uint32_t revolutions, uint16_t eventTime)
{
    return {0x01,
            static_cast<uint8_t>(revolutions & 0xFF),
            static_cast<uint8_t>((revolutions >> 8) & 0xFF),
            static_cast<uint8_t>((revolutions >> 16) & 0xFF),
            static_cast<uint8_t>((revolutions >> 24) & 0xFF),
            static_cast<uint8_t>(eventTime & 0xFF),
            static_cast<uint8_t>((eventTime >> 8) & 0xFF)};
}

/*! A CSC Measurement carrying crank data alone. */
std::vector<uint8_t> crankFrame(uint16_t revolutions, uint16_t eventTime)
{
    return {0x02,
            static_cast<uint8_t>(revolutions & 0xFF),
            static_cast<uint8_t>((revolutions >> 8) & 0xFF),
            static_cast<uint8_t>(eventTime & 0xFF),
            static_cast<uint8_t>((eventTime >> 8) & 0xFF)};
}

/*! Reads whatever is queued on a signal, returning the values. */
std::vector<double> read(const StreamReaderPtr& reader)
{
    double values[32] = {};
    SizeT count = 32;
    reader.read(values, &count);

    return std::vector<double>(values, values + count);
}

/*!
 * The GATT database is the property tree: one object property per service.
 *
 * A profile the module declines must still leave its service browsable, which is what the callers
 * of this check.
 */
daq::SizeT serviceCount(const DevicePtr& device)
{
    daq::SizeT count = 0;
    for (const auto& property : device.getAllProperties())
    {
        if (property.getValueType() == ctObject)
            ++count;
    }
    return count;
}

}

TEST(CyclingSpeedCadenceChannel, AppearsAsAChannelOnAPeripheralThatSupportsIt)
{
    const auto connected = connect(cscPeripheral());

    const auto channel = cscChannel(connected.device);
    ASSERT_TRUE(channel.assigned());
    EXPECT_EQ(channel.getName(), "CyclingSpeedCadence");
}

TEST(CyclingSpeedCadenceChannel, SubscribesToTheMeasurementCharacteristicOnConnect)
{
    const auto connected = connect(cscPeripheral());

    EXPECT_TRUE(connected.central->connection()->isSubscribed(MeasurementHandle));
}

TEST(CyclingSpeedCadenceChannel, GivesEveryValueSignalItsOwnExplicitDomain)
{
    const auto connected = connect(cscPeripheral());
    const auto channel = cscChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    std::vector<std::string> domainIds;

    for (const auto& id : {"Cadence", "CrankRevolutions", "Speed", "Distance", "WheelRevolutions"})
    {
        const auto signal = signalByLocalId(channel, id);
        ASSERT_TRUE(signal.assigned()) << id;

        const auto domain = signal.getDomainSignal();
        ASSERT_TRUE(domain.assigned()) << id;

        // Explicit is what makes a signal asynchronous: each sample carries its own timestamp.
        EXPECT_EQ(domain.getDescriptor().getRule().getType(), DataRuleType::Explicit) << id;
        EXPECT_EQ(domain.getDescriptor().getSampleType(), SampleType::Int64) << id;

        domainIds.push_back(domain.getLocalId());
    }

    // Sharing a domain would force these signals to agree on sample count, and they cannot: a
    // notification may carry wheel data, crank data or both.
    const std::set<std::string> unique(domainIds.begin(), domainIds.end());
    EXPECT_EQ(unique.size(), domainIds.size());
}

TEST(CyclingSpeedCadenceChannel, ReadsTheSensorLocationAndFeaturesOnConnect)
{
    const auto connected = connect(cscPeripheral());
    const auto channel = cscChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    EXPECT_EQ(channel.getPropertyValue("SensorLocation"), "Chainstay");
    EXPECT_EQ(channel.getPropertyValue("Features"),
              "Wheel Revolution Data, Crank Revolution Data");
}

TEST(CyclingSpeedCadenceChannel, EmitsNothingFromTheFirstNotification)
{
    auto connected = connect(cscPeripheral());
    const auto channel = cscChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto reader = dataReader(signalByLocalId(channel, "Cadence"));

    // Cadence is differentiated from a counter, so one measurement gives nothing to differentiate.
    connected.central->connection()->pushNotification(MeasurementHandle, crankFrame(100, 0));

    EXPECT_TRUE(read(reader).empty());
}

TEST(CyclingSpeedCadenceChannel, DerivesCadenceFromTheCrankCounter)
{
    auto connected = connect(cscPeripheral());
    const auto channel = cscChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto reader = dataReader(signalByLocalId(channel, "Cadence"));

    connected.central->connection()->pushNotification(MeasurementHandle, crankFrame(100, 0));
    connected.central->connection()->pushNotification(MeasurementHandle, crankFrame(101, 1024));

    const auto values = read(reader);

    ASSERT_EQ(values.size(), 1u);
    EXPECT_DOUBLE_EQ(values[0], 60.0);
}

TEST(CyclingSpeedCadenceChannel, DerivesSpeedFromAnEventTimeTickingAtOneThousandTwentyFourHertz)
{
    // The regression that matters most in this file. The identically named field in Cycling Power
    // ticks at 1/2048 s, so borrowing that rate here reports exactly twice the real speed and
    // nothing else looks wrong. At 1024 Hz one revolution in 1024 ticks is one second, so a
    // 2096 mm wheel covers 2.096 m in 1 s -- 7.5456 km/h, not 15.09.
    auto connected = connect(cscPeripheral());
    const auto channel = cscChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto reader = dataReader(signalByLocalId(channel, "Speed"));

    connected.central->connection()->pushNotification(MeasurementHandle, wheelFrame(1000, 0));
    connected.central->connection()->pushNotification(MeasurementHandle, wheelFrame(1001, 1024));

    const auto values = read(reader);

    ASSERT_EQ(values.size(), 1u);
    EXPECT_NEAR(values[0], 7.5456, 1e-9);
}

TEST(CyclingSpeedCadenceChannel, UsesTheWheelCircumferenceProperty)
{
    auto connected = connect(cscPeripheral());
    const auto channel = cscChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    // The sensor cannot know the wheel it is bolted to, so this is the one writable property.
    channel.setPropertyValue("WheelCircumferenceMm", 1048);

    auto reader = dataReader(signalByLocalId(channel, "Speed"));

    connected.central->connection()->pushNotification(MeasurementHandle, wheelFrame(1000, 0));
    connected.central->connection()->pushNotification(MeasurementHandle, wheelFrame(1001, 1024));

    const auto values = read(reader);

    ASSERT_EQ(values.size(), 1u);
    EXPECT_NEAR(values[0], 3.7728, 1e-9);
}

TEST(CyclingSpeedCadenceChannel, AccumulatesDistanceAcrossNotifications)
{
    auto connected = connect(cscPeripheral());
    const auto channel = cscChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto reader = dataReader(signalByLocalId(channel, "Distance"));

    connected.central->connection()->pushNotification(MeasurementHandle, wheelFrame(1000, 0));
    connected.central->connection()->pushNotification(MeasurementHandle, wheelFrame(1001, 1024));
    connected.central->connection()->pushNotification(MeasurementHandle, wheelFrame(1003, 2048));

    const auto values = read(reader);

    // Distance counts from the channel's first notification, not from the sensor's own counter --
    // which runs from whenever its battery went in and is typically in the millions.
    ASSERT_EQ(values.size(), 2u);
    EXPECT_NEAR(values[0], 2.096, 1e-9);
    EXPECT_NEAR(values[1], 6.288, 1e-9);
}

TEST(CyclingSpeedCadenceChannel, EmitsNoSpeedFromASensorThatOnlyReportsCadence)
{
    auto connected = connect(cscPeripheral());
    const auto channel = cscChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto speed = dataReader(signalByLocalId(channel, "Speed"));
    auto cadence = dataReader(signalByLocalId(channel, "Cadence"));

    connected.central->connection()->pushNotification(MeasurementHandle, crankFrame(100, 0));
    connected.central->connection()->pushNotification(MeasurementHandle, crankFrame(101, 1024));

    EXPECT_TRUE(read(speed).empty());
    EXPECT_EQ(read(cadence).size(), 1u);
}

TEST(CyclingSpeedCadenceChannel, IgnoresMalformedNotifications)
{
    auto connected = connect(cscPeripheral());
    const auto channel = cscChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto reader = dataReader(signalByLocalId(channel, "Cadence"));

    // Flags promise crank data with only three octets behind them.
    ASSERT_NO_THROW(connected.central->connection()->pushNotification(MeasurementHandle,
                                                                      {0x02, 0xE8, 0x03}));

    connected.central->connection()->pushNotification(MeasurementHandle, crankFrame(100, 0));
    connected.central->connection()->pushNotification(MeasurementHandle, crankFrame(101, 1024));

    EXPECT_EQ(read(reader).size(), 1u);
}

TEST(CyclingSpeedCadenceChannel, IsSkippedWhenTheMeasurementCharacteristicCannotNotify)
{
    auto peripheral = cscPeripheral();
    peripheral.services[0].characteristics[0].properties = ble::CharPropRead;

    const auto connected = connect(std::move(peripheral));

    EXPECT_EQ(connected.device.getChannels().getCount(), 0u);

    // ...and the device is still fully browsable.
    EXPECT_EQ(serviceCount(connected.device), 1u);
}

TEST(CyclingSpeedCadenceChannel, IsSkippedWhenTheServiceLacksAMeasurementCharacteristic)
{
    auto peripheral = cscPeripheral();
    peripheral.services[0].characteristics.erase(peripheral.services[0].characteristics.begin());

    const auto connected = connect(std::move(peripheral));

    EXPECT_EQ(connected.device.getChannels().getCount(), 0u);
}

TEST(CyclingSpeedCadenceChannel, UnsubscribesWhenTheDeviceGoesAway)
{
    auto owned = std::make_unique<FakeCentral>();
    auto* central = owned.get();
    central->addPeripheral("AA:BB:CC:DD:EE:FF", cscPeripheral());

    std::shared_ptr<bool> destroyed;
    {
        const auto module = createWithImplementation<IModule, BluetoothModule>(
            NullContext(), std::move(owned), instantConfig());
        const auto device = module.createDevice("bluetooth://AA:BB:CC:DD:EE:FF", nullptr);

        ASSERT_TRUE(central->connection()->isSubscribed(MeasurementHandle));
        destroyed = central->connectionDestroyedFlag();
    }

    // A channel that outlived its device would keep the sensor notifying into a dead object.
    EXPECT_TRUE(*destroyed);
}

#include "fake_ble_central.h"

#include <bluetooth_module/bluetooth_module.h>
#include <bluetooth_module/profiles/cycling_power_channel.h>

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

constexpr char MeasurementHandle[] = "/fake/cps/measurement";
constexpr char FeatureHandle[] = "/fake/cps/feature";
constexpr char LocationHandle[] = "/fake/cps/location";

PropertyObjectPtr instantConfig()
{
    auto config = BluetoothModule::createDefaultConfig();
    config.setPropertyValue("ScanWarmupMs", 0);
    return config;
}

/*! A power meter: measurement, feature and sensor location. */
FakeCentral::Peripheral powerMeterPeripheral()
{
    ble::Characteristic measurement;
    measurement.uuid = CyclingPowerChannel::measurementCharacteristicUuid();
    measurement.handle = MeasurementHandle;
    measurement.properties = ble::CharPropNotify;

    ble::Characteristic feature;
    feature.uuid = CyclingPowerChannel::featureCharacteristicUuid();
    feature.handle = FeatureHandle;
    feature.properties = ble::CharPropRead;

    ble::Characteristic location;
    location.uuid = CyclingPowerChannel::sensorLocationCharacteristicUuid();
    location.handle = LocationHandle;
    location.properties = ble::CharPropRead;

    ble::Service power;
    power.uuid = CyclingPowerChannel::serviceUuid();
    power.primary = true;
    power.characteristics = {measurement, feature, location};

    FakeCentral::Peripheral peripheral;
    peripheral.services = {power};
    // Pedal Power Balance + Crank Revolution Data supported.
    peripheral.values[FeatureHandle] = {0x09, 0x00, 0x00, 0x00};
    peripheral.values[LocationHandle] = {0x05};   // Left Crank

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

ChannelPtr powerChannel(const DevicePtr& device)
{
    for (const auto& channel : device.getChannels())
    {
        if (channel.getLocalId() == "CyclingPower")
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

StreamReaderPtr dataReader(const SignalPtr& signal)
{
    return StreamReaderBuilder()
        .setSignal(signal)
        .setValueReadType(SampleType::Float64)
        .setDomainReadType(SampleType::Int64)
        .setSkipEvents(True)
        .build();
}

std::vector<double> read(const StreamReaderPtr& reader)
{
    double values[32] = {};
    SizeT count = 32;
    reader.read(values, &count);

    return std::vector<double>(values, values + count);
}

/*! Power alone: no optional fields at all. */
std::vector<uint8_t> powerFrame(int16_t watts)
{
    const auto raw = static_cast<uint16_t>(watts);
    return {0x00, 0x00, static_cast<uint8_t>(raw & 0xFF), static_cast<uint8_t>((raw >> 8) & 0xFF)};
}

/*! Power plus crank revolution data. Crank event time ticks at 1/1024 s. */
std::vector<uint8_t> crankFrame(int16_t watts, uint16_t revolutions, uint16_t eventTime)
{
    const auto raw = static_cast<uint16_t>(watts);
    return {0x20, 0x00,
            static_cast<uint8_t>(raw & 0xFF), static_cast<uint8_t>((raw >> 8) & 0xFF),
            static_cast<uint8_t>(revolutions & 0xFF), static_cast<uint8_t>((revolutions >> 8) & 0xFF),
            static_cast<uint8_t>(eventTime & 0xFF), static_cast<uint8_t>((eventTime >> 8) & 0xFF)};
}

/*! Power plus wheel revolution data. Wheel event time ticks at 1/2048 s -- unlike the crank. */
std::vector<uint8_t> wheelFrame(int16_t watts, uint32_t revolutions, uint16_t eventTime)
{
    const auto raw = static_cast<uint16_t>(watts);
    return {0x10, 0x00,
            static_cast<uint8_t>(raw & 0xFF), static_cast<uint8_t>((raw >> 8) & 0xFF),
            static_cast<uint8_t>(revolutions & 0xFF),
            static_cast<uint8_t>((revolutions >> 8) & 0xFF),
            static_cast<uint8_t>((revolutions >> 16) & 0xFF),
            static_cast<uint8_t>((revolutions >> 24) & 0xFF),
            static_cast<uint8_t>(eventTime & 0xFF), static_cast<uint8_t>((eventTime >> 8) & 0xFF)};
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

TEST(CyclingPowerChannel, AppearsAsAChannelOnAPeripheralThatSupportsIt)
{
    const auto connected = connect(powerMeterPeripheral());

    const auto channel = powerChannel(connected.device);
    ASSERT_TRUE(channel.assigned());
    EXPECT_EQ(channel.getName(), "CyclingPower");
}

TEST(CyclingPowerChannel, SubscribesToTheMeasurementCharacteristicOnConnect)
{
    const auto connected = connect(powerMeterPeripheral());

    EXPECT_TRUE(connected.central->connection()->isSubscribed(MeasurementHandle));
}

TEST(CyclingPowerChannel, GivesEveryValueSignalItsOwnExplicitDomain)
{
    const auto connected = connect(powerMeterPeripheral());
    const auto channel = powerChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    const std::vector<std::string> ids = {
        "InstantaneousPower", "PedalPowerBalance", "AccumulatedTorque", "AccumulatedEnergy",
        "MaximumForce", "MinimumForce", "MaximumTorque", "MinimumTorque",
        "MaximumAngle", "MinimumAngle", "TopDeadSpotAngle", "BottomDeadSpotAngle",
        "Cadence", "CrankRevolutions", "Speed", "Distance", "WheelRevolutions"};

    std::set<std::string> domainIds;

    for (const auto& id : ids)
    {
        const auto signal = signalByLocalId(channel, id);
        ASSERT_TRUE(signal.assigned()) << id;

        const auto domain = signal.getDomainSignal();
        ASSERT_TRUE(domain.assigned()) << id;
        EXPECT_EQ(domain.getDescriptor().getRule().getType(), DataRuleType::Explicit) << id;

        domainIds.insert(domain.getLocalId());
    }

    // Optional fields are gated per notification, so no two of these can be assumed to produce
    // the same number of samples -- which is what sharing a domain would require.
    EXPECT_EQ(domainIds.size(), ids.size());
    EXPECT_EQ(signalByLocalId(channel, "InstantaneousPower").getDescriptor().getUnit().getSymbol(),
              "W");
}

TEST(CyclingPowerChannel, ReadsTheSensorLocationAndFeaturesOnConnect)
{
    const auto connected = connect(powerMeterPeripheral());
    const auto channel = powerChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    EXPECT_EQ(channel.getPropertyValue("SensorLocation"), "Left Crank");
    EXPECT_EQ(channel.getPropertyValue("Features"),
              "Pedal Power Balance, Crank Revolution Data");
}

TEST(CyclingPowerChannel, EmitsInstantaneousPowerForEachNotification)
{
    auto connected = connect(powerMeterPeripheral());
    const auto channel = powerChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto reader = dataReader(signalByLocalId(channel, "InstantaneousPower"));

    connected.central->connection()->pushNotification(MeasurementHandle, powerFrame(200));
    connected.central->connection()->pushNotification(MeasurementHandle, powerFrame(-30));

    const auto values = read(reader);

    ASSERT_EQ(values.size(), 2u);
    EXPECT_DOUBLE_EQ(values[0], 200.0);

    // Signed on the wire: a coasting pedal meter reports below zero.
    EXPECT_DOUBLE_EQ(values[1], -30.0);
}

TEST(CyclingPowerChannel, EmitsNoCadenceUntilThereAreTwoCrankMeasurements)
{
    auto connected = connect(powerMeterPeripheral());
    const auto channel = powerChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto reader = dataReader(signalByLocalId(channel, "Cadence"));

    connected.central->connection()->pushNotification(MeasurementHandle, crankFrame(200, 100, 0));
    EXPECT_TRUE(read(reader).empty());

    connected.central->connection()->pushNotification(MeasurementHandle, crankFrame(200, 101, 1024));

    const auto values = read(reader);
    ASSERT_EQ(values.size(), 1u);
    EXPECT_DOUBLE_EQ(values[0], 60.0);
}

TEST(CyclingPowerChannel, DerivesSpeedFromAWheelEventTimeTickingAtTwoThousandFortyEightHertz)
{
    // The Cycling Power wheel event time is 1/2048 s while its crank event time -- in the same
    // characteristic -- is 1/1024 s. Using 1024 here would report twice the real speed.
    auto connected = connect(powerMeterPeripheral());
    const auto channel = powerChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto reader = dataReader(signalByLocalId(channel, "Speed"));

    connected.central->connection()->pushNotification(MeasurementHandle, wheelFrame(200, 1000, 0));
    connected.central->connection()->pushNotification(MeasurementHandle, wheelFrame(200, 1001, 2048));

    const auto values = read(reader);

    ASSERT_EQ(values.size(), 1u);
    EXPECT_NEAR(values[0], 7.5456, 1e-9);
}

TEST(CyclingPowerChannel, UsesTheWheelCircumferenceProperty)
{
    auto connected = connect(powerMeterPeripheral());
    const auto channel = powerChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    channel.setPropertyValue("WheelCircumferenceMm", 1048);

    auto speed = dataReader(signalByLocalId(channel, "Speed"));
    auto distance = dataReader(signalByLocalId(channel, "Distance"));

    connected.central->connection()->pushNotification(MeasurementHandle, wheelFrame(200, 1000, 0));
    connected.central->connection()->pushNotification(MeasurementHandle, wheelFrame(200, 1001, 2048));

    const auto speeds = read(speed);
    ASSERT_EQ(speeds.size(), 1u);
    EXPECT_NEAR(speeds[0], 3.7728, 1e-9);

    const auto distances = read(distance);
    ASSERT_EQ(distances.size(), 1u);
    EXPECT_NEAR(distances[0], 1.048, 1e-9);
}

TEST(CyclingPowerChannel, EmitsTheOptionalFieldsANotificationCarries)
{
    auto connected = connect(powerMeterPeripheral());
    const auto channel = powerChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto balance = dataReader(signalByLocalId(channel, "PedalPowerBalance"));
    auto torque = dataReader(signalByLocalId(channel, "AccumulatedTorque"));
    auto energy = dataReader(signalByLocalId(channel, "AccumulatedEnergy"));
    auto angle = dataReader(signalByLocalId(channel, "MaximumAngle"));

    connected.central->connection()->pushNotification(
        MeasurementHandle, {0x07, 0x08,          // balance (left reference), torque, energy
                            0xC8, 0x00,          // 200 W
                            0x4B,                // balance 37.5 %
                            0x40, 0x00,          // torque 2 N.m
                            0x2C, 0x01});        // energy 300 kJ

    const auto balances = read(balance);
    ASSERT_EQ(balances.size(), 1u);
    EXPECT_DOUBLE_EQ(balances[0], 37.5);

    ASSERT_EQ(read(torque).size(), 1u);
    ASSERT_EQ(read(energy).size(), 1u);

    // Nothing promised an extreme angle, so nothing was emitted on that signal.
    EXPECT_TRUE(read(angle).empty());

    EXPECT_EQ(channel.getPropertyValue("PedalPowerBalanceReference"), "Left");
}

TEST(CyclingPowerChannel, EmitsPedallingAnalysisFieldsWhenTheMeterSendsThem)
{
    auto connected = connect(powerMeterPeripheral());
    const auto channel = powerChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto maximumForce = dataReader(signalByLocalId(channel, "MaximumForce"));
    auto maximumAngle = dataReader(signalByLocalId(channel, "MaximumAngle"));
    auto topDeadSpot = dataReader(signalByLocalId(channel, "TopDeadSpotAngle"));

    connected.central->connection()->pushNotification(
        MeasurementHandle, {0x40, 0x03,                // extreme force, extreme angles, top dead spot
                            0xC8, 0x00,                // 200 W
                            0x2C, 0x01, 0x9C, 0xFF,    // force 300 / -100 N
                            0x34, 0x12, 0xF0,          // angles 0x234 / 0xF01
                            0x1E, 0x00});              // top dead spot 30 degrees

    const auto forces = read(maximumForce);
    ASSERT_EQ(forces.size(), 1u);
    EXPECT_DOUBLE_EQ(forces[0], 300.0);

    const auto angles = read(maximumAngle);
    ASSERT_EQ(angles.size(), 1u);
    EXPECT_DOUBLE_EQ(angles[0], 564.0);

    const auto tops = read(topDeadSpot);
    ASSERT_EQ(tops.size(), 1u);
    EXPECT_DOUBLE_EQ(tops[0], 30.0);
}

TEST(CyclingPowerChannel, IgnoresMalformedNotifications)
{
    auto connected = connect(powerMeterPeripheral());
    const auto channel = powerChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto reader = dataReader(signalByLocalId(channel, "InstantaneousPower"));

    // Flags promise crank revolution data with nothing behind them.
    ASSERT_NO_THROW(
        connected.central->connection()->pushNotification(MeasurementHandle, {0x20, 0x00, 0xC8, 0x00}));

    connected.central->connection()->pushNotification(MeasurementHandle, powerFrame(200));

    const auto values = read(reader);
    ASSERT_EQ(values.size(), 1u);
    EXPECT_DOUBLE_EQ(values[0], 200.0);
}

TEST(CyclingPowerChannel, IsSkippedWhenTheMeasurementCharacteristicCannotNotify)
{
    auto peripheral = powerMeterPeripheral();
    peripheral.services[0].characteristics[0].properties = ble::CharPropRead;

    const auto connected = connect(std::move(peripheral));

    EXPECT_EQ(connected.device.getChannels().getCount(), 0u);

    EXPECT_EQ(serviceCount(connected.device), 1u);
}

TEST(CyclingPowerChannel, IsSkippedWhenTheServiceLacksAMeasurementCharacteristic)
{
    auto peripheral = powerMeterPeripheral();
    peripheral.services[0].characteristics.erase(peripheral.services[0].characteristics.begin());

    const auto connected = connect(std::move(peripheral));

    EXPECT_EQ(connected.device.getChannels().getCount(), 0u);
}

TEST(CyclingPowerChannel, UnsubscribesWhenTheDeviceGoesAway)
{
    auto owned = std::make_unique<FakeCentral>();
    auto* central = owned.get();
    central->addPeripheral("AA:BB:CC:DD:EE:FF", powerMeterPeripheral());

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

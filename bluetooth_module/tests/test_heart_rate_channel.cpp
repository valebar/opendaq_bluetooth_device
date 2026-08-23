#include "fake_ble_central.h"

#include <bluetooth_module/bluetooth_module.h>
#include <bluetooth_module/profiles/heart_rate_channel.h>

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

constexpr char MeasurementHandle[] = "/fake/hr/measurement";
constexpr char LocationHandle[] = "/fake/hr/location";
constexpr char ControlPointHandle[] = "/fake/hr/control";

PropertyObjectPtr instantConfig()
{
    auto config = BluetoothModule::createDefaultConfig();
    config.setPropertyValue("ScanWarmupMs", 0);
    return config;
}

/*! A peripheral exposing the full Heart Rate service: measurement, location and control point. */
FakeCentral::Peripheral heartRatePeripheral(bool withControlPoint = true)
{
    ble::Characteristic measurement;
    measurement.uuid = HeartRateChannel::measurementCharacteristicUuid();
    measurement.handle = MeasurementHandle;
    measurement.properties = ble::CharPropNotify;

    ble::Characteristic location;
    location.uuid = HeartRateChannel::bodySensorLocationCharacteristicUuid();
    location.handle = LocationHandle;
    location.properties = ble::CharPropRead;

    ble::Service heartRate;
    heartRate.uuid = HeartRateChannel::serviceUuid();
    heartRate.primary = true;
    heartRate.characteristics = {measurement, location};

    if (withControlPoint)
    {
        ble::Characteristic controlPoint;
        controlPoint.uuid = HeartRateChannel::controlPointCharacteristicUuid();
        controlPoint.handle = ControlPointHandle;
        controlPoint.properties = ble::CharPropWrite;
        heartRate.characteristics.push_back(controlPoint);
    }

    FakeCentral::Peripheral peripheral;
    peripheral.services = {heartRate};
    peripheral.values[LocationHandle] = {0x02};   // Wrist
    peripheral.values[ControlPointHandle] = {};

    return peripheral;
}

/*! A peripheral with a service the profile registry knows nothing about. */
FakeCentral::Peripheral unknownServicePeripheral()
{
    ble::Characteristic characteristic;
    characteristic.uuid = "00002a19-0000-1000-8000-00805f9b34fb";
    characteristic.handle = "/fake/battery/level";
    characteristic.properties = ble::CharPropRead;

    ble::Service battery;
    battery.uuid = "0000180f-0000-1000-8000-00805f9b34fb";
    battery.primary = true;
    battery.characteristics = {characteristic};

    FakeCentral::Peripheral peripheral;
    peripheral.services = {battery};
    peripheral.values["/fake/battery/level"] = {0x5A};

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

ChannelPtr heartRateChannel(const DevicePtr& device)
{
    for (const auto& channel : device.getChannels())
    {
        if (channel.getLocalId() == "HeartRate")
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

/*!
 * A reader that yields only data.
 *
 * A StreamReader stops at every event packet, and a freshly connected signal always emits a
 * descriptor-changed event first, so a plain reader returns zero samples on its first read no
 * matter how much data is waiting. Skipping events keeps the tests about the data.
 */
StreamReaderPtr dataReader(const SignalPtr& signal)
{
    return StreamReaderBuilder()
        .setSignal(signal)
        .setValueReadType(SampleType::Float64)
        .setDomainReadType(SampleType::Int64)
        .setSkipEvents(True)
        .build();
}

/*! Flags for the payloads below. */
constexpr uint8_t Uint16Format = 0x01;
constexpr uint8_t ContactSupported = 0x04;
constexpr uint8_t EnergyPresent = 0x08;
constexpr uint8_t RrPresent = 0x10;

}

TEST(HeartRateChannel, AppearsAsAChannelOnAPeripheralThatSupportsIt)
{
    const auto connected = connect(heartRatePeripheral());

    const auto channel = heartRateChannel(connected.device);
    ASSERT_TRUE(channel.assigned());
    EXPECT_EQ(channel.getName(), "HeartRate");
}

TEST(HeartRateChannel, IsAbsentOnAPeripheralWithoutTheService)
{
    const auto connected = connect(unknownServicePeripheral());

    EXPECT_EQ(connected.device.getChannels().getCount(), 0u);

    // ...and the GATT property tree is untouched by the profile machinery.
    EXPECT_TRUE(connected.device.hasProperty("Battery Service"));
}

TEST(HeartRateChannel, SubscribesToTheMeasurementCharacteristicOnConnect)
{
    const auto connected = connect(heartRatePeripheral());

    EXPECT_TRUE(connected.central->connection()->isSubscribed(MeasurementHandle));
}

TEST(HeartRateChannel, ExposesValueAndDomainSignals)
{
    const auto connected = connect(heartRatePeripheral());
    const auto channel = heartRateChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    for (const auto& id : {"HeartRate", "RrInterval", "EnergyExpended"})
        EXPECT_TRUE(signalByLocalId(channel, id).assigned()) << id;

    const auto heartRate = signalByLocalId(channel, "HeartRate");
    ASSERT_TRUE(heartRate.assigned());

    const auto domain = heartRate.getDomainSignal();
    ASSERT_TRUE(domain.assigned());

    // An asynchronous signal is one whose domain is explicit: each sample carries its own
    // timestamp instead of being placed by a sample rate.
    EXPECT_EQ(domain.getDescriptor().getRule().getType(), DataRuleType::Explicit);
    EXPECT_EQ(domain.getDescriptor().getSampleType(), SampleType::Int64);
    EXPECT_EQ(heartRate.getDescriptor().getUnit().getSymbol(), "bpm");
}

TEST(HeartRateChannel, ReadsTheBodySensorLocationOnConnect)
{
    const auto connected = connect(heartRatePeripheral());
    const auto channel = heartRateChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    EXPECT_EQ(channel.getPropertyValue("BodySensorLocation"), "Wrist");
}

TEST(HeartRateChannel, EmitsAHeartRateSampleForEachNotification)
{
    auto connected = connect(heartRatePeripheral());
    const auto channel = heartRateChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto reader = dataReader(signalByLocalId(channel, "HeartRate"));

    connected.central->connection()->pushNotification(MeasurementHandle, {0x00, 0x48});
    connected.central->connection()->pushNotification(MeasurementHandle, {0x00, 0x4B});

    double values[8] = {};
    int64_t times[8] = {};
    SizeT count = 8;
    reader.readWithDomain(values, times, &count);

    ASSERT_EQ(count, 2u);
    EXPECT_DOUBLE_EQ(values[0], 72.0);
    EXPECT_DOUBLE_EQ(values[1], 75.0);

    // Domain timestamps must advance; a consumer relies on that ordering.
    EXPECT_GT(times[1], 0);
    EXPECT_GE(times[1], times[0]);
}

TEST(HeartRateChannel, EmitsRrIntervalsOnTheirOwnSignal)
{
    auto connected = connect(heartRatePeripheral());
    const auto channel = heartRateChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto reader = dataReader(signalByLocalId(channel, "RrInterval"));

    // Heart rate 80, then RR intervals of 1.0 s and 0.5 s.
    connected.central->connection()->pushNotification(
        MeasurementHandle, {RrPresent, 0x50, 0x00, 0x04, 0x00, 0x02});

    double values[8] = {};
    int64_t times[8] = {};
    SizeT count = 8;
    reader.readWithDomain(values, times, &count);

    ASSERT_EQ(count, 2u);
    EXPECT_DOUBLE_EQ(values[0], 1.0);
    EXPECT_DOUBLE_EQ(values[1], 0.5);

    // The intervals are back-dated, so the second beat sits 0.5 s after the first.
    EXPECT_EQ(times[1] - times[0], 500000);
}

TEST(HeartRateChannel, EmitsNoRrSamplesWhenTheNotificationCarriesNone)
{
    auto connected = connect(heartRatePeripheral());
    const auto channel = heartRateChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto reader = dataReader(signalByLocalId(channel, "RrInterval"));

    connected.central->connection()->pushNotification(MeasurementHandle, {0x00, 0x48});

    double values[8] = {};
    SizeT count = 8;
    reader.read(values, &count);

    EXPECT_EQ(count, 0u);
}

TEST(HeartRateChannel, EmitsEnergyExpendedOnlyWhenReported)
{
    auto connected = connect(heartRatePeripheral());
    const auto channel = heartRateChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto reader = dataReader(signalByLocalId(channel, "EnergyExpended"));

    connected.central->connection()->pushNotification(MeasurementHandle, {0x00, 0x48});
    connected.central->connection()->pushNotification(
        MeasurementHandle, {EnergyPresent, 0x48, 0x2C, 0x01});

    double values[8] = {};
    SizeT count = 8;
    reader.read(values, &count);

    ASSERT_EQ(count, 1u);
    EXPECT_DOUBLE_EQ(values[0], 300.0);
}

TEST(HeartRateChannel, TracksSensorContactFromNotifications)
{
    auto connected = connect(heartRatePeripheral());
    const auto channel = heartRateChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    connected.central->connection()->pushNotification(
        MeasurementHandle, {static_cast<uint8_t>(ContactSupported | 0x02), 0x48});

    EXPECT_TRUE(static_cast<Bool>(channel.getPropertyValue("SensorContactSupported")));
    EXPECT_TRUE(static_cast<Bool>(channel.getPropertyValue("SensorContactDetected")));

    connected.central->connection()->pushNotification(MeasurementHandle, {ContactSupported, 0x48});

    EXPECT_FALSE(static_cast<Bool>(channel.getPropertyValue("SensorContactDetected")));
}

TEST(HeartRateChannel, IgnoresMalformedNotifications)
{
    auto connected = connect(heartRatePeripheral());
    const auto channel = heartRateChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto reader = dataReader(signalByLocalId(channel, "HeartRate"));

    // Flags promising a 16-bit value with only one octet behind them.
    ASSERT_NO_THROW(connected.central->connection()->pushNotification(MeasurementHandle, {Uint16Format, 0x36}));
    connected.central->connection()->pushNotification(MeasurementHandle, {0x00, 0x48});

    double values[8] = {};
    SizeT count = 8;
    reader.read(values, &count);

    ASSERT_EQ(count, 1u);
    EXPECT_DOUBLE_EQ(values[0], 72.0);
}

TEST(HeartRateChannel, ResetEnergyExpendedWritesTheControlPointOpcode)
{
    auto connected = connect(heartRatePeripheral());
    const auto channel = heartRateChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    const ProcedurePtr reset = channel.getPropertyValue("ResetEnergyExpended");
    reset.dispatch();

    const auto writes = connected.central->connection()->takeWrites();
    ASSERT_EQ(writes.size(), 1u);
    EXPECT_EQ(writes[0].first, ControlPointHandle);
    EXPECT_EQ(writes[0].second, (std::vector<uint8_t>{0x01}));
}

TEST(HeartRateChannel, OmitsTheResetActionWhenThePeripheralHasNoControlPoint)
{
    const auto connected = connect(heartRatePeripheral(false));
    const auto channel = heartRateChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    EXPECT_FALSE(channel.hasProperty("ResetEnergyExpended"));
}

TEST(HeartRateChannel, UnsubscribesWhenTheDeviceGoesAway)
{
    auto owned = std::make_unique<FakeCentral>();
    auto* central = owned.get();
    central->addPeripheral("AA:BB:CC:DD:EE:FF", heartRatePeripheral());

    std::shared_ptr<bool> destroyed;
    {
        const auto module = createWithImplementation<IModule, BluetoothModule>(
            NullContext(), std::move(owned), instantConfig());
        const auto device = module.createDevice("bluetooth://AA:BB:CC:DD:EE:FF", nullptr);

        ASSERT_TRUE(central->connection()->isSubscribed(MeasurementHandle));
        destroyed = central->connectionDestroyedFlag();
    }

    // The channel holds a reference to the connection, so a channel that outlived the device would
    // keep the peripheral notifying into a dead object.
    EXPECT_TRUE(*destroyed);
}

TEST(HeartRateChannel, IsSkippedWhenTheMeasurementCharacteristicCannotNotify)
{
    // A peripheral can advertise the service and still be unusable as a signal source -- typically
    // because it wants pairing before it will notify. That must cost the caller the channel, not
    // the whole device.
    auto peripheral = heartRatePeripheral();
    peripheral.services[0].characteristics[0].properties = ble::CharPropRead;

    const auto connected = connect(std::move(peripheral));

    EXPECT_EQ(connected.device.getChannels().getCount(), 0u);

    // The device is still fully browsable.
    EXPECT_TRUE(connected.device.hasProperty("Heart Rate"));
}

TEST(HeartRateChannel, IsSkippedWhenTheServiceLacksAMeasurementCharacteristic)
{
    auto peripheral = heartRatePeripheral();
    peripheral.services[0].characteristics.erase(peripheral.services[0].characteristics.begin());

    const auto connected = connect(std::move(peripheral));

    EXPECT_EQ(connected.device.getChannels().getCount(), 0u);
}

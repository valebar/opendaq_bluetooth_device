#include "fake_ble_central.h"

#include <bluetooth_module/bluetooth_module.h>
#include <bluetooth_module/profiles/fitness_machine_channel.h>

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

constexpr char IndoorBikeDataHandle[] = "/fake/ftms/indoor-bike-data";
constexpr char FeatureHandle[] = "/fake/ftms/feature";
constexpr char ControlPointHandle[] = "/fake/ftms/control-point";
constexpr char TrainingStatusHandle[] = "/fake/ftms/training-status";
constexpr char PowerRangeHandle[] = "/fake/ftms/power-range";
constexpr char ResistanceRangeHandle[] = "/fake/ftms/resistance-range";

PropertyObjectPtr instantConfig()
{
    auto config = BluetoothModule::createDefaultConfig();
    config.setPropertyValue("ScanWarmupMs", 0);
    return config;
}

/*! Target Setting Features: resistance (bit 2), power (bit 3), indoor bike simulation (bit 13). */
constexpr uint32_t AllTargets = (1u << 2) | (1u << 3) | (1u << 13);

/*!
 * A smart trainer with the full Fitness Machine service.
 *
 * @p targetSettingFeatures decides which control actions the channel is expected to offer.
 */
FakeCentral::Peripheral trainerPeripheral(uint32_t targetSettingFeatures = AllTargets,
                                          bool withControlPoint = true)
{
    ble::Characteristic indoorBikeData;
    indoorBikeData.uuid = FitnessMachineChannel::indoorBikeDataCharacteristicUuid();
    indoorBikeData.handle = IndoorBikeDataHandle;
    indoorBikeData.properties = ble::CharPropNotify;

    ble::Characteristic feature;
    feature.uuid = FitnessMachineChannel::featureCharacteristicUuid();
    feature.handle = FeatureHandle;
    feature.properties = ble::CharPropRead;

    ble::Characteristic trainingStatus;
    trainingStatus.uuid = FitnessMachineChannel::trainingStatusCharacteristicUuid();
    trainingStatus.handle = TrainingStatusHandle;
    trainingStatus.properties = ble::CharPropRead | ble::CharPropNotify;

    ble::Characteristic powerRange;
    powerRange.uuid = FitnessMachineChannel::supportedPowerRangeCharacteristicUuid();
    powerRange.handle = PowerRangeHandle;
    powerRange.properties = ble::CharPropRead;

    ble::Characteristic resistanceRange;
    resistanceRange.uuid = FitnessMachineChannel::supportedResistanceLevelRangeCharacteristicUuid();
    resistanceRange.handle = ResistanceRangeHandle;
    resistanceRange.properties = ble::CharPropRead;

    ble::Service ftms;
    ftms.uuid = FitnessMachineChannel::serviceUuid();
    ftms.primary = true;
    ftms.characteristics = {indoorBikeData, feature, trainingStatus, powerRange, resistanceRange};

    if (withControlPoint)
    {
        ble::Characteristic controlPoint;
        controlPoint.uuid = FitnessMachineChannel::controlPointCharacteristicUuid();
        controlPoint.handle = ControlPointHandle;
        controlPoint.properties = ble::CharPropWrite | ble::CharPropIndicate;
        ftms.characteristics.push_back(controlPoint);
    }

    FakeCentral::Peripheral peripheral;
    peripheral.services = {ftms};

    // Machine features: Cadence (1) and Power Measurement (14).
    peripheral.values[FeatureHandle] = {0x02, 0x40, 0x00, 0x00,
                                        static_cast<uint8_t>(targetSettingFeatures & 0xFF),
                                        static_cast<uint8_t>((targetSettingFeatures >> 8) & 0xFF),
                                        static_cast<uint8_t>((targetSettingFeatures >> 16) & 0xFF),
                                        static_cast<uint8_t>((targetSettingFeatures >> 24) & 0xFF)};
    peripheral.values[TrainingStatusHandle] = {0x00, 0x01};                    // Idle
    peripheral.values[PowerRangeHandle] = {0x00, 0x00, 0xE8, 0x03, 0x01, 0x00};  // 0..1000 W, step 1
    peripheral.values[ResistanceRangeHandle] = {0x00, 0x00, 0xFF, 0x00, 0x01, 0x00};  // 0..25.5
    peripheral.values[ControlPointHandle] = {};

    return peripheral;
}

/*! A treadmill: the same service, but its data characteristic is Treadmill Data (0x2ACD). */
FakeCentral::Peripheral treadmillPeripheral()
{
    ble::Characteristic treadmillData;
    treadmillData.uuid = "00002acd-0000-1000-8000-00805f9b34fb";
    treadmillData.handle = "/fake/ftms/treadmill-data";
    treadmillData.properties = ble::CharPropNotify;

    ble::Service ftms;
    ftms.uuid = FitnessMachineChannel::serviceUuid();
    ftms.primary = true;
    ftms.characteristics = {treadmillData};

    FakeCentral::Peripheral peripheral;
    peripheral.services = {ftms};

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

ChannelPtr fitnessMachineChannel(const DevicePtr& device)
{
    for (const auto& channel : device.getChannels())
    {
        if (channel.getLocalId() == "FitnessMachine")
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

/*! Every Indoor Bike Data value signal, in wire order. */
const std::vector<std::string>& valueSignalIds()
{
    static const std::vector<std::string> ids = {
        "Speed", "AverageSpeed", "Cadence", "AverageCadence", "TotalDistance",
        "ResistanceLevel", "InstantaneousPower", "AveragePower", "TotalEnergy",
        "EnergyPerHour", "EnergyPerMinute", "HeartRate", "MetabolicEquivalent",
        "ElapsedTime", "RemainingTime"};
    return ids;
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

TEST(FitnessMachineChannel, AppearsAsAChannelOnAnIndoorBike)
{
    const auto connected = connect(trainerPeripheral());

    const auto channel = fitnessMachineChannel(connected.device);
    ASSERT_TRUE(channel.assigned());
    EXPECT_EQ(channel.getName(), "FitnessMachine");
}

TEST(FitnessMachineChannel, IsSkippedOnATreadmill)
{
    // The Fitness Machine service is shared with treadmills, rowers and cross trainers, which
    // carry their own data characteristics. Declining is how they are politely skipped.
    const auto connected = connect(treadmillPeripheral());

    EXPECT_EQ(connected.device.getChannels().getCount(), 0u);

    // ...and the machine is still fully browsable.
    EXPECT_EQ(serviceCount(connected.device), 1u);
}

TEST(FitnessMachineChannel, SubscribesToTheDataControlPointAndTrainingStatus)
{
    const auto connected = connect(trainerPeripheral());

    EXPECT_TRUE(connected.central->connection()->isSubscribed(IndoorBikeDataHandle));

    // The control point answers by indication, so its verdict is invisible without a subscription.
    EXPECT_TRUE(connected.central->connection()->isSubscribed(ControlPointHandle));
    EXPECT_TRUE(connected.central->connection()->isSubscribed(TrainingStatusHandle));
}

TEST(FitnessMachineChannel, GivesEveryValueSignalItsOwnExplicitDomain)
{
    const auto connected = connect(trainerPeripheral());
    const auto channel = fitnessMachineChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    std::set<std::string> domainIds;

    for (const auto& id : valueSignalIds())
    {
        const auto signal = signalByLocalId(channel, id);
        ASSERT_TRUE(signal.assigned()) << id;

        const auto domain = signal.getDomainSignal();
        ASSERT_TRUE(domain.assigned()) << id;
        EXPECT_EQ(domain.getDescriptor().getRule().getType(), DataRuleType::Explicit) << id;

        domainIds.insert(domain.getLocalId());
    }

    // The service explicitly allows one data record to be split across several notifications, so
    // no two of these fields can be assumed to arrive together.
    EXPECT_EQ(domainIds.size(), valueSignalIds().size());
}

TEST(FitnessMachineChannel, PublishesTheFeatureWordsAndSupportedRanges)
{
    const auto connected = connect(trainerPeripheral());
    const auto channel = fitnessMachineChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    EXPECT_EQ(channel.getPropertyValue("Features"), "Cadence, Power Measurement");
    EXPECT_EQ(channel.getPropertyValue("TargetSettingFeatures"),
              "Resistance Target, Power Target, Indoor Bike Simulation");

    EXPECT_EQ(static_cast<Int>(channel.getPropertyValue("MinTargetPowerW")), 0);
    EXPECT_EQ(static_cast<Int>(channel.getPropertyValue("MaxTargetPowerW")), 1000);
    EXPECT_NEAR(static_cast<Float>(channel.getPropertyValue("MaxTargetResistanceLevel")), 25.5, 1e-9);

    EXPECT_EQ(channel.getPropertyValue("TrainingStatus"), "Idle");
}

TEST(FitnessMachineChannel, EmitsOnlyTheFieldsANotificationCarries)
{
    auto connected = connect(trainerPeripheral());
    const auto channel = fitnessMachineChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto speed = dataReader(signalByLocalId(channel, "Speed"));
    auto cadence = dataReader(signalByLocalId(channel, "Cadence"));
    auto power = dataReader(signalByLocalId(channel, "InstantaneousPower"));
    auto distance = dataReader(signalByLocalId(channel, "TotalDistance"));

    // Speed (bit 0 clear), cadence (bit 2) and power (bit 6).
    connected.central->connection()->pushNotification(IndoorBikeDataHandle,
                                                      {0x44, 0x00,
                                                       0xB8, 0x0B,    // 30.00 km/h
                                                       0xB4, 0x00,    // 90 rpm
                                                       0xC8, 0x00});  // 200 W

    const auto speeds = read(speed);
    ASSERT_EQ(speeds.size(), 1u);
    EXPECT_DOUBLE_EQ(speeds[0], 30.0);

    const auto cadences = read(cadence);
    ASSERT_EQ(cadences.size(), 1u);
    EXPECT_DOUBLE_EQ(cadences[0], 90.0);

    const auto powers = read(power);
    ASSERT_EQ(powers.size(), 1u);
    EXPECT_DOUBLE_EQ(powers[0], 200.0);

    // Nothing promised a distance, so nothing was emitted on that signal.
    EXPECT_TRUE(read(distance).empty());
}

TEST(FitnessMachineChannel, EmitsNoSpeedWhenTheMoreDataBitIsSet)
{
    auto connected = connect(trainerPeripheral());
    const auto channel = fitnessMachineChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto speed = dataReader(signalByLocalId(channel, "Speed"));
    auto power = dataReader(signalByLocalId(channel, "InstantaneousPower"));

    // Bit 0 set means the speed field is absent -- the one inverted flag in the characteristic.
    connected.central->connection()->pushNotification(IndoorBikeDataHandle,
                                                      {0x41, 0x00, 0xC8, 0x00});

    EXPECT_TRUE(read(speed).empty());

    const auto powers = read(power);
    ASSERT_EQ(powers.size(), 1u);
    EXPECT_DOUBLE_EQ(powers[0], 200.0);
}

TEST(FitnessMachineChannel, SkipsEnergyFieldsCarryingTheDataNotAvailableSentinel)
{
    auto connected = connect(trainerPeripheral());
    const auto channel = fitnessMachineChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto total = dataReader(signalByLocalId(channel, "TotalEnergy"));
    auto perMinute = dataReader(signalByLocalId(channel, "EnergyPerMinute"));

    connected.central->connection()->pushNotification(IndoorBikeDataHandle,
                                                      {0x01, 0x01,
                                                       0x2C, 0x01,    // 300 kcal total
                                                       0xF4, 0x01,    // 500 kcal/h
                                                       0xFF});        // per minute: not available

    ASSERT_EQ(read(total).size(), 1u);
    EXPECT_TRUE(read(perMinute).empty());
}

TEST(FitnessMachineChannel, TracksTrainingStatusFromNotifications)
{
    auto connected = connect(trainerPeripheral());
    const auto channel = fitnessMachineChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    connected.central->connection()->pushNotification(TrainingStatusHandle, {0x00, 0x0C});

    EXPECT_EQ(channel.getPropertyValue("TrainingStatus"), "Watt Control");
}

TEST(FitnessMachineChannel, IgnoresMalformedNotifications)
{
    auto connected = connect(trainerPeripheral());
    const auto channel = fitnessMachineChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    auto power = dataReader(signalByLocalId(channel, "InstantaneousPower"));

    // Flags promise power with one octet behind them.
    ASSERT_NO_THROW(
        connected.central->connection()->pushNotification(IndoorBikeDataHandle, {0x41, 0x00, 0xC8}));

    connected.central->connection()->pushNotification(IndoorBikeDataHandle,
                                                      {0x41, 0x00, 0xC8, 0x00});

    EXPECT_EQ(read(power).size(), 1u);
}

// ---- control -------------------------------------------------------------------------------

TEST(FitnessMachineChannel, OffersEveryControlActionTheTargetSettingFeaturesAllow)
{
    const auto connected = connect(trainerPeripheral());
    const auto channel = fitnessMachineChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    for (const auto* name : {"RequestControl", "Reset", "Start", "Stop", "Pause",
                             "SetTargetPower", "SetTargetResistanceLevel", "SetSimulationParameters"})
        EXPECT_TRUE(channel.hasProperty(name)) << name;
}

TEST(FitnessMachineChannel, OmitsTheActionsTheTargetSettingFeaturesWithhold)
{
    // Only Power Target. An always-present button that always fails would be worse than none.
    const auto connected = connect(trainerPeripheral(1u << 3));
    const auto channel = fitnessMachineChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    EXPECT_TRUE(channel.hasProperty("SetTargetPower"));
    EXPECT_FALSE(channel.hasProperty("SetTargetResistanceLevel"));
    EXPECT_FALSE(channel.hasProperty("SetSimulationParameters"));

    // The unconditional ones only need the control point itself.
    EXPECT_TRUE(channel.hasProperty("Start"));
}

TEST(FitnessMachineChannel, OmitsEveryControlActionWhenThereIsNoControlPoint)
{
    const auto connected = connect(trainerPeripheral(AllTargets, false));
    const auto channel = fitnessMachineChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    for (const auto* name : {"RequestControl", "Reset", "Start", "Stop", "Pause",
                             "SetTargetPower", "SetTargetResistanceLevel", "SetSimulationParameters",
                             "LastControlResult"})
        EXPECT_FALSE(channel.hasProperty(name)) << name;
}

TEST(FitnessMachineChannel, RequestControlWritesItsOpcodeWithAResponse)
{
    auto connected = connect(trainerPeripheral());
    const auto channel = fitnessMachineChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    const ProcedurePtr requestControl = channel.getPropertyValue("RequestControl");
    requestControl.dispatch();

    const auto writes = connected.central->connection()->takeWrites();
    ASSERT_EQ(writes.size(), 1u);
    EXPECT_EQ(writes[0].first, ControlPointHandle);
    EXPECT_EQ(writes[0].second, (std::vector<uint8_t>{0x00}));
    EXPECT_TRUE(connected.central->connection()->lastWriteHadResponse());
}

TEST(FitnessMachineChannel, StopAndPauseWriteDistinctControlInformation)
{
    auto connected = connect(trainerPeripheral());
    const auto channel = fitnessMachineChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    const ProcedurePtr stop = channel.getPropertyValue("Stop");
    stop.dispatch();

    const ProcedurePtr pause = channel.getPropertyValue("Pause");
    pause.dispatch();

    const auto writes = connected.central->connection()->takeWrites();
    ASSERT_EQ(writes.size(), 2u);
    EXPECT_EQ(writes[0].second, (std::vector<uint8_t>{0x08, 0x01}));
    EXPECT_EQ(writes[1].second, (std::vector<uint8_t>{0x08, 0x02}));
}

TEST(FitnessMachineChannel, SetTargetPowerWritesLittleEndianSignedWatts)
{
    auto connected = connect(trainerPeripheral());
    const auto channel = fitnessMachineChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    const ProcedurePtr setTargetPower = channel.getPropertyValue("SetTargetPower");
    setTargetPower.execute(Integer(250));

    const auto writes = connected.central->connection()->takeWrites();
    ASSERT_EQ(writes.size(), 1u);
    EXPECT_EQ(writes[0].second, (std::vector<uint8_t>{0x05, 0xFA, 0x00}));
}

TEST(FitnessMachineChannel, RejectsATargetPowerOutsideTheSupportedRangeWithoutWriting)
{
    auto connected = connect(trainerPeripheral());
    const auto channel = fitnessMachineChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    const ProcedurePtr setTargetPower = channel.getPropertyValue("SetTargetPower");

    // Rejected here, not at the trainer: the caller gets a specific error now instead of an
    // asynchronous "Invalid Parameter" it cannot tie back to a particular request.
    EXPECT_ANY_THROW(setTargetPower.execute(Integer(5000)));
    EXPECT_TRUE(connected.central->connection()->takeWrites().empty());
}

TEST(FitnessMachineChannel, SetTargetResistanceLevelWritesTenths)
{
    auto connected = connect(trainerPeripheral());
    const auto channel = fitnessMachineChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    const ProcedurePtr setResistance = channel.getPropertyValue("SetTargetResistanceLevel");
    setResistance.execute(Floating(7.5));

    const auto writes = connected.central->connection()->takeWrites();
    ASSERT_EQ(writes.size(), 1u);
    EXPECT_EQ(writes[0].second, (std::vector<uint8_t>{0x04, 0x4B}));
}

TEST(FitnessMachineChannel, SetSimulationParametersWritesSixParameterOctets)
{
    auto connected = connect(trainerPeripheral());
    const auto channel = fitnessMachineChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    const ProcedurePtr simulate = channel.getPropertyValue("SetSimulationParameters");
    simulate.execute(Floating(0.0), Floating(5.0), Floating(0.004), Floating(0.51));

    const auto writes = connected.central->connection()->takeWrites();
    ASSERT_EQ(writes.size(), 1u);
    EXPECT_EQ(writes[0].second,
              (std::vector<uint8_t>{0x11, 0x00, 0x00, 0xF4, 0x01, 0x28, 0x33}));
}

TEST(FitnessMachineChannel, RecordsTheControlPointVerdict)
{
    auto connected = connect(trainerPeripheral());
    const auto channel = fitnessMachineChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    EXPECT_EQ(channel.getPropertyValue("LastControlResult"), "None");

    connected.central->connection()->pushNotification(ControlPointHandle, {0x80, 0x05, 0x01});
    EXPECT_EQ(channel.getPropertyValue("LastControlResult"), "Set Target Power: Success");

    // The failure that matters in practice: another application took control of the trainer, and
    // without this the request simply appears to have worked.
    connected.central->connection()->pushNotification(ControlPointHandle, {0x80, 0x05, 0x05});
    EXPECT_EQ(channel.getPropertyValue("LastControlResult"),
              "Set Target Power: Control Not Permitted");
}

TEST(FitnessMachineChannel, IgnoresAnIndicationThatIsNotAResponse)
{
    auto connected = connect(trainerPeripheral());
    const auto channel = fitnessMachineChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    connected.central->connection()->pushNotification(ControlPointHandle, {0x05, 0xC8, 0x00});

    EXPECT_EQ(channel.getPropertyValue("LastControlResult"), "None");
}

TEST(FitnessMachineChannel, UnsubscribesEveryHandleWhenTheDeviceGoesAway)
{
    auto owned = std::make_unique<FakeCentral>();
    auto* central = owned.get();
    central->addPeripheral("AA:BB:CC:DD:EE:FF", trainerPeripheral());

    std::shared_ptr<bool> destroyed;
    {
        const auto module = createWithImplementation<IModule, BluetoothModule>(
            NullContext(), std::move(owned), instantConfig());
        const auto device = module.createDevice("bluetooth://AA:BB:CC:DD:EE:FF", nullptr);

        const auto channel = fitnessMachineChannel(device);
        ASSERT_TRUE(channel.assigned());

        // Calling every action first: a cycle created by *invoking* a callable is invisible to a
        // test that only constructs one.
        ProcedurePtr(channel.getPropertyValue("RequestControl")).dispatch();
        ProcedurePtr(channel.getPropertyValue("Start")).dispatch();
        ProcedurePtr(channel.getPropertyValue("SetTargetPower")).execute(Integer(200));
        ProcedurePtr(channel.getPropertyValue("SetTargetResistanceLevel")).execute(Floating(5.0));
        ProcedurePtr(channel.getPropertyValue("SetSimulationParameters"))
            .execute(Floating(0.0), Floating(1.0), Floating(0.004), Floating(0.51));
        ProcedurePtr(channel.getPropertyValue("Stop")).dispatch();

        destroyed = central->connectionDestroyedFlag();
    }

    // Three subscriptions, and a channel holding a reference to itself would release none of them.
    EXPECT_TRUE(*destroyed);
}

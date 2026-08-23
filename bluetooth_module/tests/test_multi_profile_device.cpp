#include "fake_ble_central.h"

#include <bluetooth_module/bluetooth_module.h>
#include <bluetooth_module/profiles/cycling_power_channel.h>
#include <bluetooth_module/profiles/cycling_speed_cadence_channel.h>
#include <bluetooth_module/profiles/fitness_machine_channel.h>
#include <bluetooth_module/profiles/heart_rate_channel.h>

#include <gtest/gtest.h>

#include <opendaq/context_factory.h>
#include <opendaq/search_filter_factory.h>

#include <memory>
#include <set>
#include <string>

using namespace daq;
using namespace daq::modules::bluetooth_module;
using opendaq_bt::testing::FakeCentral;

namespace ble = opendaq_bt::ble;

namespace
{

constexpr char HeartRateHandle[] = "/fake/hr/measurement";
constexpr char PowerHandle[] = "/fake/cps/measurement";
constexpr char CscHandle[] = "/fake/csc/measurement";
constexpr char IndoorBikeHandle[] = "/fake/ftms/indoor-bike-data";
constexpr char FitnessFeatureHandle[] = "/fake/ftms/feature";

PropertyObjectPtr instantConfig()
{
    auto config = BluetoothModule::createDefaultConfig();
    config.setPropertyValue("ScanWarmupMs", 0);
    return config;
}

ble::Service notifyingService(const char* serviceUuid,
                              const char* characteristicUuid,
                              const char* handle)
{
    ble::Characteristic measurement;
    measurement.uuid = characteristicUuid;
    measurement.handle = handle;
    measurement.properties = ble::CharPropNotify;

    ble::Service service;
    service.uuid = serviceUuid;
    service.primary = true;
    service.characteristics = {measurement};

    return service;
}

/*!
 * A peripheral shaped like a real smart trainer.
 *
 * A Wahoo KICKR or a Tacx Neo hosts the Fitness Machine service *and* Cycling Power *and* Cycling
 * Speed and Cadence at once, and a strap paired through it adds Heart Rate. Four profiles on one
 * link is the ordinary case for this hardware, not a contrived one.
 */
FakeCentral::Peripheral smartTrainerPeripheral()
{
    auto fitnessMachine = notifyingService(FitnessMachineChannel::serviceUuid(),
                                           FitnessMachineChannel::indoorBikeDataCharacteristicUuid(),
                                           IndoorBikeHandle);

    ble::Characteristic feature;
    feature.uuid = FitnessMachineChannel::featureCharacteristicUuid();
    feature.handle = FitnessFeatureHandle;
    feature.properties = ble::CharPropRead;
    fitnessMachine.characteristics.push_back(feature);

    FakeCentral::Peripheral peripheral;
    peripheral.services = {
        notifyingService(HeartRateChannel::serviceUuid(),
                         HeartRateChannel::measurementCharacteristicUuid(), HeartRateHandle),
        notifyingService(CyclingPowerChannel::serviceUuid(),
                         CyclingPowerChannel::measurementCharacteristicUuid(), PowerHandle),
        notifyingService(CyclingSpeedCadenceChannel::serviceUuid(),
                         CyclingSpeedCadenceChannel::measurementCharacteristicUuid(), CscHandle),
        fitnessMachine};

    peripheral.values[FitnessFeatureHandle] = {0x02, 0x40, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00};

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

std::set<std::string> channelIds(const DevicePtr& device)
{
    std::set<std::string> ids;
    for (const auto& channel : device.getChannels())
        ids.insert(channel.getLocalId());
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

TEST(MultiProfileDevice, BuildsOneChannelPerProfileThePeripheralHosts)
{
    const auto connected = connect(smartTrainerPeripheral());

    EXPECT_EQ(channelIds(connected.device),
              (std::set<std::string>{"HeartRate", "CyclingPower", "CyclingSpeedCadence",
                                     "FitnessMachine"}));
}

TEST(MultiProfileDevice, SubscribesToEveryMeasurementCharacteristic)
{
    const auto connected = connect(smartTrainerPeripheral());

    for (const auto* handle : {HeartRateHandle, PowerHandle, CscHandle, IndoorBikeHandle})
        EXPECT_TRUE(connected.central->connection()->isSubscribed(handle)) << handle;
}

TEST(MultiProfileDevice, ChannelsHaveIndependentSignalNamespaces)
{
    const auto connected = connect(smartTrainerPeripheral());

    // Cycling Power and Cycling Speed and Cadence both own a signal called WheelRevolutions, and
    // the Fitness Machine channel owns another Cadence. Local ids are scoped to their channel, so
    // the three coexist -- but nothing else in the module says so out loud.
    ChannelPtr power;
    ChannelPtr csc;
    for (const auto& channel : connected.device.getChannels())
    {
        if (channel.getLocalId() == "CyclingPower")
            power = channel;
        if (channel.getLocalId() == "CyclingSpeedCadence")
            csc = channel;
    }

    ASSERT_TRUE(power.assigned());
    ASSERT_TRUE(csc.assigned());

    const auto find = [](const ChannelPtr& channel, const char* localId) {
        for (const auto& signal : channel.getSignals(search::Any()))
        {
            if (signal.getLocalId() == localId)
                return signal;
        }
        return SignalPtr();
    };

    const auto fromPower = find(power, "WheelRevolutions");
    const auto fromCsc = find(csc, "WheelRevolutions");

    ASSERT_TRUE(fromPower.assigned());
    ASSERT_TRUE(fromCsc.assigned());
    EXPECT_NE(fromPower.getGlobalId(), fromCsc.getGlobalId());
}

TEST(MultiProfileDevice, AnUnusableProfileDoesNotCostTheOthers)
{
    auto peripheral = smartTrainerPeripheral();

    // The CSC measurement cannot notify -- typically a sensor that wants pairing first.
    peripheral.services[2].characteristics[0].properties = ble::CharPropRead;

    const auto connected = connect(std::move(peripheral));

    EXPECT_EQ(channelIds(connected.device),
              (std::set<std::string>{"HeartRate", "CyclingPower", "FitnessMachine"}));

    // ...and every service is still browsable, decoded or not.
    EXPECT_EQ(serviceCount(connected.device), 4u);
}

TEST(MultiProfileDevice, ReleasesTheConnectionWhenTheDeviceGoesAway)
{
    auto owned = std::make_unique<FakeCentral>();
    auto* central = owned.get();
    central->addPeripheral("AA:BB:CC:DD:EE:FF", smartTrainerPeripheral());

    std::shared_ptr<bool> destroyed;
    {
        const auto module = createWithImplementation<IModule, BluetoothModule>(
            NullContext(), std::move(owned), instantConfig());
        const auto device = module.createDevice("bluetooth://AA:BB:CC:DD:EE:FF", nullptr);

        ASSERT_EQ(device.getChannels().getCount(), 4u);
        destroyed = central->connectionDestroyedFlag();
    }

    // Four channels each holding the connection: one of them leaking keeps the link open.
    EXPECT_TRUE(*destroyed);
}

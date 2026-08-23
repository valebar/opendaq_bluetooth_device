#pragma once

#include <bluetooth_module/ble/ble_central.h>
#include <bluetooth_module/common.h>
#include <bluetooth_module/profiles/channel_support.h>
#include <bluetooth_module/profiles/indoor_bike_data.h>

#include <opendaq/channel_impl.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

BEGIN_NAMESPACE_OPENDAQ_BLUETOOTH

/*!
 * The Bluetooth SIG Fitness Machine service (0x1826), presented as an openDAQ channel.
 *
 * This is the smart trainer profile, and the only one in this module that writes as well as reads:
 * besides streaming Indoor Bike Data it exposes the Fitness Machine Control Point, so a trainer
 * can be put in ERG mode at a target wattage, given a resistance level, or driven from simulated
 * road gradient.
 *
 * Only indoor bikes are modelled. The Fitness Machine service is shared with treadmills, rowers
 * and cross trainers, which carry their own data characteristics; a machine without Indoor Bike
 * Data (0x2AD2) is declined, and `buildProfileChannels` logs that and leaves the device otherwise
 * fully browsable.
 *
 * **Control is asynchronous, and must stay that way.** A procedure writes its request and returns;
 * the trainer's verdict arrives later as an indication and lands in the read-only
 * LastControlResult property. Blocking a procedure until the indication arrived would look like
 * a nicer API and would deadlock: the procedure holds `sync` while waiting, and the indication
 * callback needs `sync` to record the result.
 */
class FitnessMachineChannel final : public ChannelImpl<>
{
public:
    FitnessMachineChannel(const ContextPtr& context,
                          const ComponentPtr& parent,
                          const StringPtr& localId,
                          std::shared_ptr<::opendaq_bt::ble::IConnection> connection,
                          const ::opendaq_bt::ble::Service& service);

    ~FitnessMachineChannel() override;

    /*! UUIDs of the Fitness Machine service and its characteristics, in full 128-bit lowercase. */
    static const char* serviceUuid();
    static const char* indoorBikeDataCharacteristicUuid();
    static const char* featureCharacteristicUuid();
    static const char* trainingStatusCharacteristicUuid();
    static const char* supportedResistanceLevelRangeCharacteristicUuid();
    static const char* supportedPowerRangeCharacteristicUuid();
    static const char* controlPointCharacteristicUuid();

private:
    void initProperties(const ::opendaq_bt::ble::Service& service);
    void addControlActions();
    void createSignals();

    void readFeature(const std::string& handle);
    void readPowerRange(const std::string& handle);
    void readResistanceRange(const std::string& handle);
    void readTrainingStatus(const std::string& handle);

    /*! Subscribes and remembers the handle, so the destructor cannot drift out of step. */
    void subscribeTo(const std::string& handle,
                     const std::function<void(const std::vector<uint8_t>&)>& handler);

    void onIndoorBikeData(const std::vector<uint8_t>& payload);
    void onControlPointResponse(const std::vector<uint8_t>& payload);
    void onTrainingStatus(const std::vector<uint8_t>& payload);

    /*! Sends one control point request. Throws an openDAQ exception if the link cannot take it. */
    void writeControlPoint(const std::vector<uint8_t>& request);

    /*! Microseconds since the Unix epoch, anchored once and advanced by a steady clock. */
    int64_t nowTicks() const;

    /*! Shared so the connection cannot be torn down while this channel still uses it. */
    const std::shared_ptr<::opendaq_bt::ble::IConnection> connection;

    std::string indoorBikeDataHandle;
    std::string controlPointHandle;

    /*! Every handle subscribed, in subscription order, so teardown releases all of them. */
    std::vector<std::string> subscribedHandles;

    const std::chrono::microseconds epochOffset;
    const std::chrono::steady_clock::time_point steadyStart;

    profile_support::SignalPair speed;
    profile_support::SignalPair averageSpeed;
    profile_support::SignalPair cadence;
    profile_support::SignalPair averageCadence;
    profile_support::SignalPair totalDistance;
    profile_support::SignalPair resistanceLevel;
    profile_support::SignalPair power;
    profile_support::SignalPair averagePower;
    profile_support::SignalPair totalEnergy;
    profile_support::SignalPair energyPerHour;
    profile_support::SignalPair energyPerMinute;
    profile_support::SignalPair heartRate;
    profile_support::SignalPair metabolicEquivalent;
    profile_support::SignalPair elapsedTime;
    profile_support::SignalPair remainingTime;

    /*! What the trainer says it can be told to do. Gates which control actions exist at all. */
    ::opendaq_bt::profiles::FitnessMachineFeature features;

    /*! Bounds for SetTargetPower, when the trainer publishes them. */
    bool powerRangeKnown = false;
    ::opendaq_bt::profiles::SupportedRange powerRange;

    /*! Bounds for SetTargetResistanceLevel, when the trainer publishes them. */
    bool resistanceRangeKnown = false;
    ::opendaq_bt::profiles::SupportedRange resistanceRange;

    /*!
     * Set as the first act of the destructor.
     *
     * A control procedure is a property value, so an application can hold a ProcedurePtr after
     * dropping every reference to the channel; calling it then would write through a freed
     * `this`. Every procedure and every notification handler takes `sync` and checks this.
     */
    std::atomic<bool> shuttingDown{false};

    /*! Last published training status, so an unchanged one does not re-notify every listener. */
    uint8_t lastTrainingStatus = 0xFF;

    /*! Serialises packet emission and control writes; notifications arrive on a backend thread. */
    std::mutex sync;
};

END_NAMESPACE_OPENDAQ_BLUETOOTH

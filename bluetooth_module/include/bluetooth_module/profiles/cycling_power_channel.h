#pragma once

#include <bluetooth_module/ble/ble_central.h>
#include <bluetooth_module/common.h>
#include <bluetooth_module/profiles/channel_support.h>
#include <bluetooth_module/profiles/revolution_tracker.h>

#include <opendaq/channel_impl.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

BEGIN_NAMESPACE_OPENDAQ_BLUETOOTH

/*!
 * The Bluetooth SIG Cycling Power service (0x1818), presented as an openDAQ channel.
 *
 * This is the power meter profile: crank, pedal, hub and trainer power meters all speak it, and so
 * do most smart trainers alongside the Fitness Machine service. Instantaneous power is the only
 * mandatory field; everything else -- pedal balance, torque, crank and wheel motion, energy --
 * appears only when the sensor sends it, which it announces per notification.
 *
 * Cadence and speed are *not* transmitted. The service sends cumulative revolution counters with
 * the timestamp of the last revolution, and this channel differentiates them through
 * RevolutionTracker. That means the first notification after connecting produces no cadence: there
 * is nothing to differentiate it against yet.
 */
class CyclingPowerChannel final : public ChannelImpl<>
{
public:
    CyclingPowerChannel(const ContextPtr& context,
                        const ComponentPtr& parent,
                        const StringPtr& localId,
                        std::shared_ptr<::opendaq_bt::ble::IConnection> connection,
                        const ::opendaq_bt::ble::Service& service);

    ~CyclingPowerChannel() override;

    /*! UUIDs of the Cycling Power service and its characteristics, in full 128-bit lowercase. */
    static const char* serviceUuid();
    static const char* measurementCharacteristicUuid();
    static const char* featureCharacteristicUuid();
    static const char* sensorLocationCharacteristicUuid();

private:
    void initProperties(const ::opendaq_bt::ble::Service& service);
    void createSignals();
    void readFeature(const std::string& handle);
    void readSensorLocation(const std::string& handle);

    void onMeasurement(const std::vector<uint8_t>& payload);

    /*! Microseconds since the Unix epoch, anchored once and advanced by a steady clock. */
    int64_t nowTicks() const;

    /*! Shared so the connection cannot be torn down while this channel still uses it. */
    const std::shared_ptr<::opendaq_bt::ble::IConnection> connection;
    std::string measurementHandle;

    const std::chrono::microseconds epochOffset;
    const std::chrono::steady_clock::time_point steadyStart;

    profile_support::SignalPair power;
    profile_support::SignalPair pedalBalance;
    profile_support::SignalPair torque;
    profile_support::SignalPair energy;
    profile_support::SignalPair maximumForce;
    profile_support::SignalPair minimumForce;
    profile_support::SignalPair maximumTorque;
    profile_support::SignalPair minimumTorque;
    profile_support::SignalPair maximumAngle;
    profile_support::SignalPair minimumAngle;
    profile_support::SignalPair topDeadSpotAngle;
    profile_support::SignalPair bottomDeadSpotAngle;
    profile_support::SignalPair cadence;
    profile_support::SignalPair crankRevolutions;
    profile_support::SignalPair speed;
    profile_support::SignalPair distance;
    profile_support::SignalPair wheelRevolutions;

    // Crank counters are uint16 and tick at 1/1024 s; wheel counters are uint32 and tick at
    // 1/2048 s. That asymmetry is inside one characteristic and is the easiest mistake to make
    // in this profile -- it produces a speed that is exactly twice reality, and nothing else.
    ::opendaq_bt::profiles::RevolutionTracker crankTracker{
        {uint64_t{1} << 16, 1024.0, 5.0, 3.0}};
    ::opendaq_bt::profiles::RevolutionTracker wheelTracker{
        {uint64_t{1} << 32, 2048.0, 70.0, 3.0}};

    double sessionCrankRevolutions = 0.0;
    double sessionWheelRevolutions = 0.0;
    double sessionDistanceMetres = 0.0;

    /*!
     * Mirror of the WheelCircumferenceMm property, so the notification path never calls into
     * openDAQ's property system on a backend thread just to read a number.
     */
    std::atomic<int> wheelCircumferenceMm{2096};

    /*!
     * Last published flag-derived strings. Setting a property notifies every listener, and these
     * two are constant for the life of most sensors, so they are written only when they change.
     */
    bool balanceReferencePublished = false;
    bool lastBalanceReferenceIsLeft = false;
    bool torqueSourcePublished = false;
    bool lastTorqueIsCrankBased = false;

    /*! Serialises packet emission; notifications arrive on a backend thread. */
    std::mutex sync;
};

END_NAMESPACE_OPENDAQ_BLUETOOTH

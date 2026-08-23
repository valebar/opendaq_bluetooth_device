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
 * The Bluetooth SIG Cycling Speed and Cadence service (0x1816), presented as an openDAQ channel.
 *
 * The profile spoken by the small magnet-and-sensor pods that clip to a chainstay or crank arm. A
 * sensor may report wheel motion, crank motion, or both; a cadence-only pod sends crank data
 * alone and this channel simply produces nothing on the speed signals.
 *
 * As in Cycling Power, neither speed nor cadence is transmitted -- both are differentiated from
 * cumulative counters here -- so the first notification after connecting produces no samples.
 * Unlike Cycling Power, *both* event times tick at 1/1024 s.
 */
class CyclingSpeedCadenceChannel final : public ChannelImpl<>
{
public:
    CyclingSpeedCadenceChannel(const ContextPtr& context,
                               const ComponentPtr& parent,
                               const StringPtr& localId,
                               std::shared_ptr<::opendaq_bt::ble::IConnection> connection,
                               const ::opendaq_bt::ble::Service& service);

    ~CyclingSpeedCadenceChannel() override;

    /*! UUIDs of the CSC service and its characteristics, in full 128-bit lowercase. */
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

    profile_support::SignalPair cadence;
    profile_support::SignalPair crankRevolutions;
    profile_support::SignalPair speed;
    profile_support::SignalPair distance;
    profile_support::SignalPair wheelRevolutions;

    // Crank counters are uint16, wheel counters uint32, and -- unlike Cycling Power, whose wheel
    // event time ticks at 1/2048 s -- BOTH tick at 1/1024 s here. Copying the CPS rates across
    // reports exactly twice the real speed and nothing else looks wrong.
    ::opendaq_bt::profiles::RevolutionTracker crankTracker{
        {uint64_t{1} << 16, 1024.0, 5.0, 3.0}};
    ::opendaq_bt::profiles::RevolutionTracker wheelTracker{
        {uint64_t{1} << 32, 1024.0, 70.0, 3.0}};

    double sessionCrankRevolutions = 0.0;
    double sessionWheelRevolutions = 0.0;
    double sessionDistanceMetres = 0.0;

    /*! Mirror of WheelCircumferenceMm, so the notification path takes no openDAQ property lock. */
    std::atomic<int> wheelCircumferenceMm{2096};

    /*! Serialises packet emission; notifications arrive on a backend thread. */
    std::mutex sync;
};

END_NAMESPACE_OPENDAQ_BLUETOOTH

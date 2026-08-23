#pragma once

#include <bluetooth_module/ble/ble_central.h>
#include <bluetooth_module/common.h>

#include <opendaq/channel_impl.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>

BEGIN_NAMESPACE_OPENDAQ_BLUETOOTH

/*!
 * The Bluetooth SIG Heart Rate service (0x180D), presented as an openDAQ channel.
 *
 * Every signal here is asynchronous: the domain uses an explicit rule, so each sample carries its
 * own timestamp rather than being placed by a sample rate. This matches GATT
 * notifications, which arrive when the peripheral decides -- roughly once a second, but not on a
 * clock -- and which may carry several RR intervals or none.
 *
 * The three value signals each have their own domain signal because their sample counts differ per
 * notification: there is always exactly one heart rate value, but zero-or-many RR intervals and an
 * energy figure only when the peripheral reports one. Signals sharing a domain must agree on
 * count, so they cannot be merged.
 */
class HeartRateChannel final : public ChannelImpl<>
{
public:
    HeartRateChannel(const ContextPtr& context,
                     const ComponentPtr& parent,
                     const StringPtr& localId,
                     std::shared_ptr<::opendaq_bt::ble::IConnection> connection,
                     const ::opendaq_bt::ble::Service& service);

    ~HeartRateChannel() override;

    /*! UUIDs of the Heart Rate service and its characteristics, as full 128-bit lowercase forms. */
    static const char* serviceUuid();
    static const char* measurementCharacteristicUuid();
    static const char* bodySensorLocationCharacteristicUuid();
    static const char* controlPointCharacteristicUuid();

private:
    void initProperties(const ::opendaq_bt::ble::Service& service);
    void createSignals();
    void buildSignalDescriptors();
    void readBodySensorLocation(const std::string& handle);

    /*! Decodes one notification payload and emits packets on whichever signals it feeds. */
    void onMeasurement(const std::vector<uint8_t>& payload);

    /*!
     * Domain timestamp for right now, in microseconds since the Unix epoch.
     *
     * Anchored to the wall clock once at construction and advanced by a steady clock thereafter.
     * Reading system_clock per notification would let an NTP correction step the domain backwards,
     * which breaks the monotonicity every openDAQ consumer assumes of a domain signal.
     */
    int64_t nowTicks() const;

    /*! Shared so the connection cannot be torn down while this channel still uses it. */
    const std::shared_ptr<::opendaq_bt::ble::IConnection> connection;
    std::string measurementHandle;
    std::string controlPointHandle;

    const std::chrono::microseconds epochOffset;
    const std::chrono::steady_clock::time_point steadyStart;

    SignalConfigPtr heartRateSignal;
    SignalConfigPtr heartRateTimeSignal;
    SignalConfigPtr rrIntervalSignal;
    SignalConfigPtr rrIntervalTimeSignal;
    SignalConfigPtr energySignal;
    SignalConfigPtr energyTimeSignal;

    /*! Serialises packet emission; notifications arrive on a backend thread. */
    std::mutex sync;
};

END_NAMESPACE_OPENDAQ_BLUETOOTH

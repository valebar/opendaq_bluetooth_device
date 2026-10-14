#pragma once

#include <bluetooth_module/ble/ble_central.h>
#include <bluetooth_module/common.h>
#include <bluetooth_module/profiles/channel_support.h>

#include <opendaq/channel_impl.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

BEGIN_NAMESPACE_OPENDAQ_BLUETOOTH

/*!
 * The Bluetooth SIG Environmental Sensing service (0x181A), presented as an openDAQ channel.
 *
 * Only Temperature (0x2A6E) is decoded. ESS allows several instances of it -- indoor and outdoor,
 * say -- so each notifying one gets its own signal: Temperature, Temperature2, ... in GATT order.
 * Notify is optional in ESS; a read-only temperature would need polling and is skipped.
 */
class EnvironmentalSensingChannel final : public ChannelImpl<>
{
public:
    EnvironmentalSensingChannel(const ContextPtr& context,
                                const ComponentPtr& parent,
                                const StringPtr& localId,
                                std::shared_ptr<::opendaq_bt::ble::IConnection> connection,
                                const ::opendaq_bt::ble::Service& service);

    ~EnvironmentalSensingChannel() override;

    /*! UUIDs of the Environmental Sensing service and its characteristics, in full 128-bit lowercase. */
    static const char* serviceUuid();
    static const char* temperatureCharacteristicUuid();

private:
    profile_support::SignalPair createTemperatureSignal(size_t index);

    void onTemperature(const std::vector<uint8_t>& payload, size_t index);

    /*! Microseconds since the Unix epoch, anchored once and advanced by a steady clock. */
    int64_t nowTicks() const;

    /*! Shared so the connection cannot be torn down while this channel still uses it. */
    const std::shared_ptr<::opendaq_bt::ble::IConnection> connection;
    std::vector<std::string> subscribedHandles;

    const std::chrono::microseconds epochOffset;
    const std::chrono::steady_clock::time_point steadyStart;

    /*! Sized before the first subscription and never resized, so callbacks can index it freely. */
    std::vector<profile_support::SignalPair> temperatures;

    /*! Serialises packet emission; notifications arrive on a backend thread. */
    std::mutex sync;
};

END_NAMESPACE_OPENDAQ_BLUETOOTH

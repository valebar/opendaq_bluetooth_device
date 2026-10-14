#pragma once

#include <bluetooth_module/ble/ble_central.h>
#include <bluetooth_module/common.h>
#include <bluetooth_module/profiles/channel_support.h>

#include <opendaq/channel_impl.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

BEGIN_NAMESPACE_OPENDAQ_BLUETOOTH

/*!
 * The Bluetooth SIG Health Thermometer service (0x1809), presented as an openDAQ channel.
 *
 * Temperature Measurement is indicated, typically once per Measurement Interval. Intermediate
 * Temperature, when present, streams while a reading settles and gets its own signal so it never
 * mixes with final readings. Both are published in degC whatever unit the peripheral uses.
 */
class HealthThermometerChannel final : public ChannelImpl<>
{
public:
    HealthThermometerChannel(const ContextPtr& context,
                             const ComponentPtr& parent,
                             const StringPtr& localId,
                             std::shared_ptr<::opendaq_bt::ble::IConnection> connection,
                             const ::opendaq_bt::ble::Service& service);

    ~HealthThermometerChannel() override;

    /*! UUIDs of the Health Thermometer service and its characteristics, in full 128-bit lowercase. */
    static const char* serviceUuid();
    static const char* measurementCharacteristicUuid();
    static const char* temperatureTypeCharacteristicUuid();
    static const char* intermediateTemperatureCharacteristicUuid();

private:
    void initProperties(const ::opendaq_bt::ble::Service& service);
    void createSignals();
    void subscribeIntermediate(const ::opendaq_bt::ble::Service& service);

    void onMeasurement(const std::vector<uint8_t>& payload,
                       const profile_support::SignalPair& target,
                       const char* characteristicName);

    /*! Writes TemperatureType only when it changed. Call with @c sync held, or before subscribing. */
    void publishTemperatureType(uint8_t value);

    /*! Microseconds since the Unix epoch, anchored once and advanced by a steady clock. */
    int64_t nowTicks() const;

    /*! Shared so the connection cannot be torn down while this channel still uses it. */
    const std::shared_ptr<::opendaq_bt::ble::IConnection> connection;
    std::vector<std::string> subscribedHandles;

    const std::chrono::microseconds epochOffset;
    const std::chrono::steady_clock::time_point steadyStart;

    profile_support::SignalPair temperature;
    profile_support::SignalPair intermediateTemperature;

    std::optional<uint8_t> temperatureType;

    /*! Serialises packet emission; notifications arrive on a backend thread. */
    std::mutex sync;
};

END_NAMESPACE_OPENDAQ_BLUETOOTH

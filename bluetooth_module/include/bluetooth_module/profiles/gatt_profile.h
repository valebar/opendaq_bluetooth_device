#pragma once

#include <bluetooth_module/ble/ble_central.h>
#include <bluetooth_module/common.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

BEGIN_NAMESPACE_OPENDAQ_BLUETOOTH

/*!
 * A GATT service this module understands well enough to turn into openDAQ signals.
 *
 * Browsing the GATT tree works for any peripheral, but producing *measurements* requires knowing
 * what the bytes of a characteristic mean. A profile is that knowledge for one service: it claims
 * a service UUID and, when a connected peripheral offers it, builds a channel carrying the decoded
 * signals.
 *
 * Peripherals whose services match no profile are unaffected -- they simply get no channels.
 */
struct GattProfile
{
    /*! Full 128-bit lowercase UUID, as produced by ble::expandUuid. */
    std::string serviceUuid;

    /*! Display name for the channel, e.g. "Heart Rate". */
    std::string name;

    /*! openDAQ local id for the channel, e.g. "HeartRate". Must be free of punctuation. */
    std::string channelLocalId;

    /*!
     * Builds the channel. May throw to decline a service that is present but unusable -- for
     * instance one missing the characteristic that carries the measurement.
     */
    std::function<ChannelPtr(const ContextPtr& context,
                             const ComponentPtr& parent,
                             const StringPtr& localId,
                             const std::shared_ptr<::opendaq_bt::ble::IConnection>& connection,
                             const ::opendaq_bt::ble::Service& service)>
        createChannel;
};

/*! Every profile this module implements, in no particular order. */
const std::vector<GattProfile>& gattProfiles();

/*! The profile claiming @p serviceUuid, or nullptr. Spelling-insensitive. */
const GattProfile* findGattProfile(const std::string& serviceUuid);

/*!
 * Whether this advertisement claims a service some profile in the registry can decode.
 *
 * Strict: an advertisement carrying no service UUIDs at all does not match. Discovery uses this to
 * decide what to list, so widening the registry widens discovery too.
 */
bool advertisesKnownGattProfile(const ::opendaq_bt::ble::Advertisement& advertisement);

END_NAMESPACE_OPENDAQ_BLUETOOTH

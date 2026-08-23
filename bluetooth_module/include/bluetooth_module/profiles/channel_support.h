#pragma once

#include <bluetooth_module/ble/ble_central.h>
#include <bluetooth_module/common.h>

#include <opendaq/packet_factory.h>
#include <opendaq/signal_factory.h>

#include <chrono>
#include <cstdint>
#include <string>

BEGIN_NAMESPACE_OPENDAQ_BLUETOOTH

/*!
 * Pieces every profile channel needs.
 *
 * The four cycling and heart rate channels all model the same thing -- a GATT characteristic that
 * notifies, decoded into asynchronous openDAQ signals -- so the domain descriptor, the epoch
 * anchor and the characteristic lookup are identical in each. They live here rather than being
 * copied, because they are exactly the details that would drift apart silently.
 */
namespace profile_support
{

/*! Domain ticks are microseconds throughout the module. Ample for signals updating about 1 Hz. */
inline constexpr int64_t TicksPerSecond = 1000000;

RatioPtr tickResolution();

/*! openDAQ wants an ISO-8601 origin; every timestamp this module emits is Unix-epoch relative. */
StringPtr epochOrigin();

/*!
 * The wall clock, once.
 *
 * Channels anchor to this at construction and advance by a steady clock thereafter. Reading the
 * system clock per notification would let an NTP correction step the domain backwards, breaking
 * the monotonicity every openDAQ consumer assumes of a domain signal.
 */
std::chrono::microseconds currentEpochOffset();

/*!
 * An explicit-rule Int64 microsecond domain descriptor.
 *
 * Explicit is what makes a signal asynchronous: each sample carries its own timestamp instead of
 * being placed according to a fixed sample rate.
 */
DataDescriptorPtr domainDescriptor(const char* name, int64_t minDeltaTicks, int64_t maxDeltaTicks);

/*! The handle of the characteristic in @p service with @p uuid, or an empty string. */
std::string handleOf(const ::opendaq_bt::ble::Service& service, const char* uuid);

/*! The characteristic in @p service with @p uuid, or nullptr. Spelling-insensitive. */
const ::opendaq_bt::ble::Characteristic* characteristicOf(const ::opendaq_bt::ble::Service& service,
                                                          const char* uuid);

/*!
 * A value signal and the domain signal that timestamps it.
 *
 * Every value signal in these profiles gets its own domain signal, because field presence is
 * per-notification flag-gated: two values decoded from the same characteristic routinely produce
 * different sample counts, and signals sharing a domain must agree on count. Pairing them in one
 * object is what stops that rule from turning fifteen signals into thirty lines of bookkeeping.
 */
struct SignalPair
{
    SignalConfigPtr value;
    SignalConfigPtr domain;

    /*! Emits one sample on both signals. Domain first, so the timestamp is never the late one. */
    void send(int64_t tick, double sample) const
    {
        const auto domainPacket = DataPacket(domain.getDescriptor(), 1);
        const auto dataPacket = DataPacketWithDomain(domainPacket, value.getDescriptor(), 1);

        *static_cast<int64_t*>(domainPacket.getRawData()) = tick;
        *static_cast<double*>(dataPacket.getRawData()) = sample;

        domain.sendPacket(domainPacket);
        value.sendPacket(dataPacket);
    }
};

}

END_NAMESPACE_OPENDAQ_BLUETOOTH

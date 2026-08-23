#include <bluetooth_module/ble/ble_types.h>
#include <bluetooth_module/profiles/channel_support.h>

#include <coreobjects/unit_factory.h>
#include <opendaq/data_rule_factory.h>

BEGIN_NAMESPACE_OPENDAQ_BLUETOOTH

namespace profile_support
{

namespace ble = ::opendaq_bt::ble;

RatioPtr tickResolution()
{
    return Ratio(1, TicksPerSecond);
}

StringPtr epochOrigin()
{
    return "1970-01-01T00:00:00Z";
}

std::chrono::microseconds currentEpochOffset()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch());
}

DataDescriptorPtr domainDescriptor(const char* name, int64_t minDeltaTicks, int64_t maxDeltaTicks)
{
    return DataDescriptorBuilder()
        .setName(name)
        .setSampleType(SampleType::Int64)
        .setUnit(Unit("s", -1, "seconds", "time"))
        .setTickResolution(tickResolution())
        .setOrigin(epochOrigin())
        .setRule(ExplicitDomainDataRule(minDeltaTicks, maxDeltaTicks))
        .build();
}

const ble::Characteristic* characteristicOf(const ble::Service& service, const char* uuid)
{
    // Both sides are expanded, so a backend reporting the 16-bit shorthand and one reporting the
    // full 128-bit form resolve to the same characteristic.
    const auto wanted = ble::expandUuid(uuid);

    for (const auto& characteristic : service.characteristics)
    {
        if (ble::expandUuid(characteristic.uuid) == wanted)
            return &characteristic;
    }

    return nullptr;
}

std::string handleOf(const ble::Service& service, const char* uuid)
{
    const auto* characteristic = characteristicOf(service, uuid);
    return characteristic == nullptr ? std::string{} : characteristic->handle;
}

}

END_NAMESPACE_OPENDAQ_BLUETOOTH

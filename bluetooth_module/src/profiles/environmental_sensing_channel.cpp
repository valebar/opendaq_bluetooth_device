#include <bluetooth_module/ble/ble_types.h>
#include <bluetooth_module/profiles/environmental_sensing_channel.h>
#include <bluetooth_module/profiles/temperature_measurement.h>

#include <coreobjects/unit_factory.h>
#include <opendaq/custom_log.h>
#include <opendaq/signal_factory.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

BEGIN_NAMESPACE_OPENDAQ_BLUETOOTH

namespace ble = ::opendaq_bt::ble;
namespace profiles = ::opendaq_bt::profiles;

using namespace std::chrono;
using profile_support::TicksPerSecond;

namespace
{

/*! ESS update intervals are device-chosen and often minutes apart. */
constexpr int64_t MinExpectedDeltaTicks = TicksPerSecond / 10;   // 100 ms
constexpr int64_t MaxExpectedDeltaTicks = TicksPerSecond * 3600; // 1 h

/*! Escaped so a compiler without /utf-8 keeps the bytes. */
constexpr char DegreesCelsius[] = "\xC2\xB0" "C";

}

const char* EnvironmentalSensingChannel::serviceUuid()
{
    return "0000181a-0000-1000-8000-00805f9b34fb";
}

const char* EnvironmentalSensingChannel::temperatureCharacteristicUuid()
{
    return "00002a6e-0000-1000-8000-00805f9b34fb";
}

EnvironmentalSensingChannel::EnvironmentalSensingChannel(const ContextPtr& context,
                                                         const ComponentPtr& parent,
                                                         const StringPtr& localId,
                                                         std::shared_ptr<ble::IConnection> connection,
                                                         const ble::Service& service)
    : ChannelImpl(FunctionBlockType("BluetoothEnvironmentalSensing",
                                    "Environmental Sensing",
                                    "Bluetooth SIG Environmental Sensing service"),
                  context,
                  parent,
                  localId)
    , connection(std::move(connection))
    , epochOffset(profile_support::currentEpochOffset())
    , steadyStart(steady_clock::now())
{
    const auto wanted = ble::expandUuid(temperatureCharacteristicUuid());

    std::vector<std::string> handles;
    for (const auto& characteristic : service.characteristics)
    {
        if (ble::expandUuid(characteristic.uuid) != wanted)
            continue;

        if ((characteristic.properties & (ble::CharPropNotify | ble::CharPropIndicate)) == 0)
        {
            LOG_I("Skipping a Temperature characteristic that does not notify")
            continue;
        }

        handles.push_back(characteristic.handle);
    }

    if (handles.empty())
        DAQ_THROW_EXCEPTION(NotFoundException,
                            "The Environmental Sensing service exposes no notifying Temperature "
                            "characteristic.");

    for (size_t i = 0; i < handles.size(); ++i)
        temperatures.push_back(createTemperatureSignal(i));

    // Each subscription is best-effort: a throw after an earlier one took would leave that one
    // notifying into a destroyed object, because a throwing constructor runs no destructor.
    for (size_t i = 0; i < handles.size(); ++i)
    {
        try
        {
            this->connection->subscribe(handles[i], [this, i](const std::vector<uint8_t>& payload) {
                onTemperature(payload, i);
            });
            subscribedHandles.push_back(handles[i]);
        }
        catch (const std::exception& e)
        {
            LOG_W("Cannot subscribe to Temperature {}: {}", i + 1, e.what())
        }
    }

    if (subscribedHandles.empty())
        DAQ_THROW_EXCEPTION(InvalidStateException,
                            "No Temperature characteristic accepted a subscription.");
}

EnvironmentalSensingChannel::~EnvironmentalSensingChannel()
{
    // Each handle released separately, so a throw on one cannot leave the others notifying.
    for (const auto& handle : subscribedHandles)
    {
        try
        {
            connection->unsubscribe(handle);
        }
        catch (...)
        {
            // The link may already be down. catch (...) because an exception escaping a
            // destructor is std::terminate().
        }
    }
}

profile_support::SignalPair EnvironmentalSensingChannel::createTemperatureSignal(size_t index)
{
    const auto suffix = index == 0 ? std::string{} : std::to_string(index + 1);
    const auto localId = "Temperature" + suffix;
    const auto name = index == 0 ? std::string("Temperature") : "Temperature " + suffix;

    profile_support::SignalPair result;
    result.value = createAndAddSignal(localId);
    result.domain = createAndAddSignal(localId + "Time", nullptr, false, false);

    result.value.setDescriptor(DataDescriptorBuilder()
                                   .setName(name)
                                   .setSampleType(SampleType::Float64)
                                   .setUnit(Unit(DegreesCelsius, -1, "degree Celsius", "temperature"))
                                   .build());
    result.domain.setDescriptor(
        profile_support::domainDescriptor(name.c_str(), MinExpectedDeltaTicks, MaxExpectedDeltaTicks));
    result.value.setDomainSignal(result.domain);

    return result;
}

int64_t EnvironmentalSensingChannel::nowTicks() const
{
    const auto elapsed = duration_cast<microseconds>(steady_clock::now() - steadyStart);
    return (epochOffset + elapsed).count();
}

void EnvironmentalSensingChannel::onTemperature(const std::vector<uint8_t>& payload, size_t index)
{
    const auto decoded = profiles::decodeEnvironmentalTemperature(payload);
    if (!decoded)
    {
        LOG_W("Discarding a malformed Temperature of {} byte(s)", payload.size())
        return;
    }

    // The "unknown" sentinel produces no sample rather than a fake -327.68 degC.
    if (!decoded->celsius)
        return;

    const auto arrival = nowTicks();

    std::scoped_lock lock(sync);
    temperatures[index].send(arrival, *decoded->celsius);
}

END_NAMESPACE_OPENDAQ_BLUETOOTH

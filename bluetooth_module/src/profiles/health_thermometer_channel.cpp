#include <bluetooth_module/ble/ble_types.h>
#include <bluetooth_module/profiles/health_thermometer_channel.h>
#include <bluetooth_module/profiles/temperature_measurement.h>

#include <coreobjects/property_object_protected_ptr.h>
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

/*! Intermediate readings stream about once a second; Measurement Interval can be many minutes. */
constexpr int64_t MinExpectedDeltaTicks = TicksPerSecond / 10;   // 100 ms
constexpr int64_t MaxExpectedDeltaTicks = TicksPerSecond * 3600; // 1 h

/*! Escaped so a compiler without /utf-8 keeps the bytes. */
constexpr char DegreesCelsius[] = "\xC2\xB0" "C";

bool canNotify(const ble::Characteristic& characteristic)
{
    return (characteristic.properties & (ble::CharPropNotify | ble::CharPropIndicate)) != 0;
}

}

const char* HealthThermometerChannel::serviceUuid()
{
    return "00001809-0000-1000-8000-00805f9b34fb";
}

const char* HealthThermometerChannel::measurementCharacteristicUuid()
{
    return "00002a1c-0000-1000-8000-00805f9b34fb";
}

const char* HealthThermometerChannel::temperatureTypeCharacteristicUuid()
{
    return "00002a1d-0000-1000-8000-00805f9b34fb";
}

const char* HealthThermometerChannel::intermediateTemperatureCharacteristicUuid()
{
    return "00002a1e-0000-1000-8000-00805f9b34fb";
}

HealthThermometerChannel::HealthThermometerChannel(const ContextPtr& context,
                                                   const ComponentPtr& parent,
                                                   const StringPtr& localId,
                                                   std::shared_ptr<ble::IConnection> connection,
                                                   const ble::Service& service)
    : ChannelImpl(FunctionBlockType("BluetoothHealthThermometer",
                                    "Health Thermometer",
                                    "Bluetooth SIG Health Thermometer service"),
                  context,
                  parent,
                  localId)
    , connection(std::move(connection))
    , epochOffset(profile_support::currentEpochOffset())
    , steadyStart(steady_clock::now())
{
    const auto* measurement = profile_support::characteristicOf(service, measurementCharacteristicUuid());
    if (measurement == nullptr)
        DAQ_THROW_EXCEPTION(NotFoundException,
                            "The Health Thermometer service does not expose a Temperature "
                            "Measurement characteristic.");

    if (!canNotify(*measurement))
        DAQ_THROW_EXCEPTION(InvalidStateException,
                            "The Temperature Measurement characteristic does not support indications.");

    initProperties(service);
    createSignals();

    // Mandatory first: if it throws nothing is subscribed yet, so declining the channel is clean.
    this->connection->subscribe(measurement->handle, [this](const std::vector<uint8_t>& payload) {
        onMeasurement(payload, temperature, "Temperature Measurement");
    });
    subscribedHandles.push_back(measurement->handle);

    subscribeIntermediate(service);
}

HealthThermometerChannel::~HealthThermometerChannel()
{
    // Each handle released separately, so a throw on one cannot leave the other notifying.
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

void HealthThermometerChannel::initProperties(const ble::Service& service)
{
    const auto objPtr = this->template borrowPtr<PropertyObjectPtr>();

    objPtr.addProperty(StringPropertyBuilder("TemperatureType", "Unknown").setReadOnly(True).build());

    const auto typeHandle = profile_support::handleOf(service, temperatureTypeCharacteristicUuid());
    if (typeHandle.empty())
        return;

    try
    {
        const auto value = connection->read(typeHandle);
        if (!value.empty())
            publishTemperatureType(value[0]);
    }
    catch (const std::exception& e)
    {
        // Purely descriptive; a thermometer that refuses the read still measures.
        LOG_D("Cannot read the temperature type: {}", e.what())
    }
}

void HealthThermometerChannel::subscribeIntermediate(const ble::Service& service)
{
    const auto* intermediate =
        profile_support::characteristicOf(service, intermediateTemperatureCharacteristicUuid());
    if (intermediate == nullptr || !canNotify(*intermediate))
        return;

    // Optional, so a refusal must not throw past the subscription above and leave it dangling.
    try
    {
        connection->subscribe(intermediate->handle, [this](const std::vector<uint8_t>& payload) {
            onMeasurement(payload, intermediateTemperature, "Intermediate Temperature");
        });
        subscribedHandles.push_back(intermediate->handle);
    }
    catch (const std::exception& e)
    {
        LOG_W("Cannot subscribe to Intermediate Temperature: {}", e.what())
    }
}

void HealthThermometerChannel::createSignals()
{
    const auto pair = [this](const char* localId, const char* name) {
        profile_support::SignalPair result;
        result.value = createAndAddSignal(localId);
        result.domain = createAndAddSignal(std::string(localId) + "Time", nullptr, false, false);

        result.value.setDescriptor(DataDescriptorBuilder()
                                       .setName(name)
                                       .setSampleType(SampleType::Float64)
                                       .setUnit(Unit(DegreesCelsius, -1, "degree Celsius", "temperature"))
                                       .build());
        result.domain.setDescriptor(
            profile_support::domainDescriptor(name, MinExpectedDeltaTicks, MaxExpectedDeltaTicks));
        result.value.setDomainSignal(result.domain);

        return result;
    };

    temperature = pair("Temperature", "Temperature");
    intermediateTemperature = pair("IntermediateTemperature", "Intermediate Temperature");
}

void HealthThermometerChannel::publishTemperatureType(uint8_t value)
{
    if (temperatureType == value)
        return;

    temperatureType = value;
    this->template borrowPtr<PropertyObjectProtectedPtr>().setProtectedPropertyValue(
        String("TemperatureType"), String(profiles::temperatureTypeName(value)));
}

int64_t HealthThermometerChannel::nowTicks() const
{
    const auto elapsed = duration_cast<microseconds>(steady_clock::now() - steadyStart);
    return (epochOffset + elapsed).count();
}

void HealthThermometerChannel::onMeasurement(const std::vector<uint8_t>& payload,
                                             const profile_support::SignalPair& target,
                                             const char* characteristicName)
{
    const auto decoded = profiles::decodeTemperatureMeasurement(payload);
    if (!decoded)
    {
        LOG_W("Discarding a malformed {} of {} byte(s)", characteristicName, payload.size())
        return;
    }

    const auto arrival = nowTicks();

    std::scoped_lock lock(sync);

    if (decoded->temperatureType)
        publishTemperatureType(*decoded->temperatureType);

    // A well-formed "no reading" (NaN, NRes, INF) produces no sample rather than a fake one.
    if (decoded->celsius)
        target.send(arrival, *decoded->celsius);
}

END_NAMESPACE_OPENDAQ_BLUETOOTH

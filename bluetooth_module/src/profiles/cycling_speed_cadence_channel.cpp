#include <bluetooth_module/ble/ble_types.h>
#include <bluetooth_module/profiles/cycling_common.h>
#include <bluetooth_module/profiles/cycling_speed_cadence_channel.h>
#include <bluetooth_module/profiles/cycling_speed_cadence_measurement.h>

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

/*!
 * Expected spacing between samples, in ticks. CSC sensors notify about once a second, but a
 * stationary sensor stops producing samples entirely, so the upper bound is generous.
 */
constexpr int64_t MinExpectedDeltaTicks = TicksPerSecond / 50;   // 20 ms
constexpr int64_t MaxExpectedDeltaTicks = TicksPerSecond * 10;   // 10 s

/*! A 700x25c tyre, the most common road wheel. Only speed and distance depend on it. */
constexpr int DefaultWheelCircumferenceMm = 2096;

double speedKph(double revolutionsPerMinute, int circumferenceMm)
{
    return revolutionsPerMinute * circumferenceMm * 60.0 / 1000000.0;
}

}

const char* CyclingSpeedCadenceChannel::serviceUuid()
{
    return "00001816-0000-1000-8000-00805f9b34fb";
}

const char* CyclingSpeedCadenceChannel::measurementCharacteristicUuid()
{
    return "00002a5b-0000-1000-8000-00805f9b34fb";
}

const char* CyclingSpeedCadenceChannel::featureCharacteristicUuid()
{
    return "00002a5c-0000-1000-8000-00805f9b34fb";
}

const char* CyclingSpeedCadenceChannel::sensorLocationCharacteristicUuid()
{
    return "00002a5d-0000-1000-8000-00805f9b34fb";
}

CyclingSpeedCadenceChannel::CyclingSpeedCadenceChannel(const ContextPtr& context,
                                                       const ComponentPtr& parent,
                                                       const StringPtr& localId,
                                                       std::shared_ptr<ble::IConnection> connection,
                                                       const ble::Service& service)
    : ChannelImpl(FunctionBlockType("BluetoothCyclingSpeedCadence",
                                    "Cycling Speed and Cadence",
                                    "Bluetooth SIG Cycling Speed and Cadence service"),
                  context,
                  parent,
                  localId)
    , connection(std::move(connection))
    , epochOffset(profile_support::currentEpochOffset())
    , steadyStart(steady_clock::now())
{
    const auto* measurement =
        profile_support::characteristicOf(service, measurementCharacteristicUuid());
    if (measurement == nullptr)
        DAQ_THROW_EXCEPTION(NotFoundException,
                            "The Cycling Speed and Cadence service does not expose a CSC "
                            "Measurement characteristic.");

    if ((measurement->properties & (ble::CharPropNotify | ble::CharPropIndicate)) == 0)
        DAQ_THROW_EXCEPTION(InvalidStateException,
                            "The CSC Measurement characteristic does not support notifications.");

    measurementHandle = measurement->handle;

    initProperties(service);
    createSignals();

    this->connection->subscribe(measurementHandle,
                                [this](const std::vector<uint8_t>& payload) { onMeasurement(payload); });
}

CyclingSpeedCadenceChannel::~CyclingSpeedCadenceChannel()
{
    try
    {
        connection->unsubscribe(measurementHandle);
    }
    catch (...)
    {
        // The link may already be down; there is nothing useful to do about it during teardown.
        // Deliberately catch everything: a WinRT hresult_error does not derive from
        // std::exception, and an exception escaping a destructor is std::terminate().
    }
}

void CyclingSpeedCadenceChannel::initProperties(const ble::Service& service)
{
    const auto objPtr = this->template borrowPtr<PropertyObjectPtr>();

    objPtr.addProperty(StringPropertyBuilder("SensorLocation", "Unknown").setReadOnly(True).build());
    objPtr.addProperty(StringPropertyBuilder("Features", "Unknown").setReadOnly(True).build());

    // The sensor cannot know the wheel it is fitted to; speed and distance are undefined without.
    objPtr.addProperty(IntProperty("WheelCircumferenceMm", DefaultWheelCircumferenceMm));

    objPtr.getOnPropertyValueWrite("WheelCircumferenceMm") +=
        [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args) {
            wheelCircumferenceMm = static_cast<int>(static_cast<Int>(args.getValue()));
        };

    const auto locationHandle = profile_support::handleOf(service, sensorLocationCharacteristicUuid());
    if (!locationHandle.empty())
    {
        readSensorLocation(locationHandle);
    }

    const auto featureHandle = profile_support::handleOf(service, featureCharacteristicUuid());
    if (!featureHandle.empty())
    {
        readFeature(featureHandle);
    }
}

void CyclingSpeedCadenceChannel::readSensorLocation(const std::string& handle)
{
    try
    {
        const auto value = connection->read(handle);
        if (value.empty())
            return;

        this->template borrowPtr<PropertyObjectProtectedPtr>().setProtectedPropertyValue(
            String("SensorLocation"), String(profiles::cyclingSensorLocationName(value[0])));
    }
    catch (const std::exception& e)
    {
        // Purely descriptive; a sensor that refuses the read still streams cadence.
        LOG_D("Cannot read the CSC sensor location: {}", e.what())
    }
}

void CyclingSpeedCadenceChannel::readFeature(const std::string& handle)
{
    try
    {
        const auto value = connection->read(handle);
        if (value.size() < 2)
            return;

        const auto features =
            static_cast<uint16_t>(static_cast<uint16_t>(value[0]) |
                                  static_cast<uint16_t>(static_cast<uint16_t>(value[1]) << 8));

        this->template borrowPtr<PropertyObjectProtectedPtr>().setProtectedPropertyValue(
            String("Features"), String(profiles::describeCscFeatures(features)));
    }
    catch (const std::exception& e)
    {
        LOG_D("Cannot read the CSC feature set: {}", e.what())
    }
}

void CyclingSpeedCadenceChannel::createSignals()
{
    // One domain signal per value signal: a notification may carry wheel data, crank data or both,
    // so the per-notification sample counts differ and cannot share a domain.
    const auto pair = [this](const char* localId,
                             const char* name,
                             const UnitPtr& unit,
                             const RangePtr& range) {
        profile_support::SignalPair result;
        result.value = createAndAddSignal(localId);
        result.domain = createAndAddSignal(std::string(localId) + "Time", nullptr, false, false);

        auto descriptor = DataDescriptorBuilder()
                              .setName(name)
                              .setSampleType(SampleType::Float64)
                              .setUnit(unit);
        if (range.assigned())
            descriptor.setValueRange(range);

        result.value.setDescriptor(descriptor.build());
        result.domain.setDescriptor(profile_support::domainDescriptor(
            name, MinExpectedDeltaTicks, MaxExpectedDeltaTicks));
        result.value.setDomainSignal(result.domain);

        return result;
    };

    const auto revolutions = Unit("rev", -1, "revolutions", "count");

    cadence = pair("Cadence", "Cadence",
                   Unit("rpm", -1, "revolutions per minute", "angular velocity"), Range(0, 250));
    crankRevolutions = pair("CrankRevolutions", "Crank Revolutions", revolutions, nullptr);
    speed = pair("Speed", "Speed", Unit("km/h", -1, "kilometres per hour", "velocity"), nullptr);
    distance = pair("Distance", "Distance", Unit("m", -1, "metre", "length"), nullptr);
    wheelRevolutions = pair("WheelRevolutions", "Wheel Revolutions", revolutions, nullptr);
}

int64_t CyclingSpeedCadenceChannel::nowTicks() const
{
    const auto elapsed = duration_cast<microseconds>(steady_clock::now() - steadyStart);
    return (epochOffset + elapsed).count();
}

void CyclingSpeedCadenceChannel::onMeasurement(const std::vector<uint8_t>& payload)
{
    const auto decoded = profiles::decodeCscMeasurement(payload);
    if (!decoded)
    {
        LOG_W("Discarding a malformed CSC Measurement of {} byte(s)", payload.size())
        return;
    }

    const auto arrival = nowTicks();

    std::scoped_lock lock(sync);

    if (decoded->crankRevolutions)
    {
        const auto update = crankTracker.update(decoded->crankRevolutions->cumulativeRevolutions,
                                                decoded->crankRevolutions->lastEventTime,
                                                arrival);
        if (update)
        {
            cadence.send(arrival, update->revolutionsPerMinute);

            // A coast-down carries no revolutions, so republishing the unchanged cumulative count
            // would only add a duplicate sample to a signal consumers integrate.
            if (!update->coastDown)
            {
                sessionCrankRevolutions += update->revolutions;
                crankRevolutions.send(arrival, sessionCrankRevolutions);
            }
        }
    }

    if (decoded->wheelRevolutions)
    {
        const auto update = wheelTracker.update(decoded->wheelRevolutions->cumulativeRevolutions,
                                                decoded->wheelRevolutions->lastEventTime,
                                                arrival);
        if (update)
        {
            const auto circumference = wheelCircumferenceMm.load();

            speed.send(arrival, speedKph(update->revolutionsPerMinute, circumference));

            if (!update->coastDown)
            {
                sessionWheelRevolutions += update->revolutions;
                sessionDistanceMetres += update->revolutions * circumference / 1000.0;

                distance.send(arrival, sessionDistanceMetres);
                wheelRevolutions.send(arrival, sessionWheelRevolutions);
            }
        }
    }
}

END_NAMESPACE_OPENDAQ_BLUETOOTH

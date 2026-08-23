#include <bluetooth_module/ble/ble_types.h>
#include <bluetooth_module/profiles/cycling_common.h>
#include <bluetooth_module/profiles/cycling_power_channel.h>
#include <bluetooth_module/profiles/cycling_power_measurement.h>

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
 * Expected spacing between samples, in ticks. Cycling Power notifies about once a second; the
 * bounds are loose because a trainer under load may notify faster and a coasting sensor slower.
 */
constexpr int64_t MinExpectedDeltaTicks = TicksPerSecond / 50;   // 20 ms
constexpr int64_t MaxExpectedDeltaTicks = TicksPerSecond * 10;   // 10 s

/*! A 700x25c tyre, the most common road wheel. Only speed and distance depend on it. */
constexpr int DefaultWheelCircumferenceMm = 2096;

/*! rev/min of a wheel of @p circumferenceMm, as km/h. */
double speedKph(double revolutionsPerMinute, int circumferenceMm)
{
    return revolutionsPerMinute * circumferenceMm * 60.0 / 1000000.0;
}

}

const char* CyclingPowerChannel::serviceUuid()
{
    return "00001818-0000-1000-8000-00805f9b34fb";
}

const char* CyclingPowerChannel::measurementCharacteristicUuid()
{
    return "00002a63-0000-1000-8000-00805f9b34fb";
}

const char* CyclingPowerChannel::featureCharacteristicUuid()
{
    return "00002a65-0000-1000-8000-00805f9b34fb";
}

const char* CyclingPowerChannel::sensorLocationCharacteristicUuid()
{
    return "00002a5d-0000-1000-8000-00805f9b34fb";
}

CyclingPowerChannel::CyclingPowerChannel(const ContextPtr& context,
                                         const ComponentPtr& parent,
                                         const StringPtr& localId,
                                         std::shared_ptr<ble::IConnection> connection,
                                         const ble::Service& service)
    : ChannelImpl(FunctionBlockType("BluetoothCyclingPower",
                                    "Cycling Power",
                                    "Bluetooth SIG Cycling Power service"),
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
                            "The Cycling Power service does not expose a Cycling Power "
                            "Measurement characteristic.");

    // Without notifications there is nothing to stream, which is all this channel is for.
    if ((measurement->properties & (ble::CharPropNotify | ble::CharPropIndicate)) == 0)
        DAQ_THROW_EXCEPTION(InvalidStateException,
                            "The Cycling Power Measurement characteristic does not support "
                            "notifications.");

    measurementHandle = measurement->handle;

    initProperties(service);
    createSignals();

    this->connection->subscribe(measurementHandle,
                                [this](const std::vector<uint8_t>& payload) { onMeasurement(payload); });
}

CyclingPowerChannel::~CyclingPowerChannel()
{
    // Stop the peripheral notifying before the signals go away, so no packet is ever built against
    // a half-destroyed channel. An exception escaping a destructor is std::terminate().
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

void CyclingPowerChannel::initProperties(const ble::Service& service)
{
    const auto objPtr = this->template borrowPtr<PropertyObjectPtr>();

    // Mirrors of sensor state: read-only to applications, updated internally through the
    // protected setter as notifications arrive.
    objPtr.addProperty(StringPropertyBuilder("SensorLocation", "Unknown").setReadOnly(True).build());
    objPtr.addProperty(StringPropertyBuilder("Features", "Unknown").setReadOnly(True).build());
    objPtr.addProperty(
        StringPropertyBuilder("PedalPowerBalanceReference", "Unknown").setReadOnly(True).build());
    objPtr.addProperty(
        StringPropertyBuilder("AccumulatedTorqueSource", "Unknown").setReadOnly(True).build());

    // The one genuinely writable property here: the sensor cannot know the wheel it is fitted to,
    // and speed and distance are meaningless without it.
    objPtr.addProperty(IntProperty("WheelCircumferenceMm", DefaultWheelCircumferenceMm));

    // Mirrored into an atomic so the notification path never takes openDAQ's property lock on a
    // backend thread. Capturing a raw this is deliberate: the handler lives on this very object,
    // so a smart pointer would be a reference cycle no destructor could break.
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

void CyclingPowerChannel::readSensorLocation(const std::string& handle)
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
        // Purely descriptive; a sensor that refuses the read still streams power.
        LOG_D("Cannot read the cycling power sensor location: {}", e.what())
    }
}

void CyclingPowerChannel::readFeature(const std::string& handle)
{
    try
    {
        const auto value = connection->read(handle);
        if (value.size() < 4)
            return;

        const auto features = static_cast<uint32_t>(value[0]) |
                              (static_cast<uint32_t>(value[1]) << 8) |
                              (static_cast<uint32_t>(value[2]) << 16) |
                              (static_cast<uint32_t>(value[3]) << 24);

        this->template borrowPtr<PropertyObjectProtectedPtr>().setProtectedPropertyValue(
            String("Features"), String(profiles::describeCyclingPowerFeatures(features)));
    }
    catch (const std::exception& e)
    {
        LOG_D("Cannot read the cycling power feature set: {}", e.what())
    }
}

void CyclingPowerChannel::createSignals()
{
    // Each value signal gets its own domain signal. Field presence is per-notification, so two
    // values decoded from the same characteristic routinely produce different sample counts, and
    // signals sharing a domain must agree on count.
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
    const auto newtons = Unit("N", -1, "newton", "force");
    const auto newtonMetres = Unit("N.m", -1, "newton metre", "torque");
    const auto degrees = Unit("deg", -1, "degree", "angle");

    power = pair("InstantaneousPower", "Instantaneous Power",
                 Unit("W", -1, "watt", "power"), Range(-2000, 3000));
    pedalBalance = pair("PedalPowerBalance", "Pedal Power Balance",
                        Unit("%", -1, "percent", "ratio"), Range(0, 100));
    torque = pair("AccumulatedTorque", "Accumulated Torque", newtonMetres, nullptr);
    energy = pair("AccumulatedEnergy", "Accumulated Energy",
                  Unit("kJ", -1, "kilojoule", "energy"), nullptr);

    // Pedalling-analysis fields. Few meters send them, but the decoder has to read them anyway to
    // keep the later offsets right, so dropping them here would discard data already in hand.
    maximumForce = pair("MaximumForce", "Maximum Force Magnitude", newtons, nullptr);
    minimumForce = pair("MinimumForce", "Minimum Force Magnitude", newtons, nullptr);
    maximumTorque = pair("MaximumTorque", "Maximum Torque Magnitude", newtonMetres, nullptr);
    minimumTorque = pair("MinimumTorque", "Minimum Torque Magnitude", newtonMetres, nullptr);
    maximumAngle = pair("MaximumAngle", "Maximum Angle", degrees, Range(0, 360));
    minimumAngle = pair("MinimumAngle", "Minimum Angle", degrees, Range(0, 360));
    topDeadSpotAngle = pair("TopDeadSpotAngle", "Top Dead Spot Angle", degrees, Range(0, 360));
    bottomDeadSpotAngle =
        pair("BottomDeadSpotAngle", "Bottom Dead Spot Angle", degrees, Range(0, 360));

    cadence = pair("Cadence", "Cadence",
                   Unit("rpm", -1, "revolutions per minute", "angular velocity"), Range(0, 250));
    crankRevolutions = pair("CrankRevolutions", "Crank Revolutions", revolutions, nullptr);
    speed = pair("Speed", "Speed", Unit("km/h", -1, "kilometres per hour", "velocity"), nullptr);
    distance = pair("Distance", "Distance", Unit("m", -1, "metre", "length"), nullptr);
    wheelRevolutions = pair("WheelRevolutions", "Wheel Revolutions", revolutions, nullptr);
}

int64_t CyclingPowerChannel::nowTicks() const
{
    const auto elapsed = duration_cast<microseconds>(steady_clock::now() - steadyStart);
    return (epochOffset + elapsed).count();
}

void CyclingPowerChannel::onMeasurement(const std::vector<uint8_t>& payload)
{
    const auto decoded = profiles::decodeCyclingPowerMeasurement(payload);
    if (!decoded)
    {
        LOG_W("Discarding a malformed Cycling Power Measurement of {} byte(s)", payload.size())
        return;
    }

    const auto arrival = nowTicks();

    std::scoped_lock lock(sync);

    const auto objPtr = this->template borrowPtr<PropertyObjectProtectedPtr>();

    // These two flags qualify the fields beside them, so they mean nothing when that field is
    // absent. They are written only on a real change: setting a property notifies every listener,
    // and a sensor's answer to both is constant for its whole life.
    if (decoded->pedalPowerBalancePercent &&
        (!balanceReferencePublished ||
         decoded->pedalPowerBalanceReferenceIsLeft != lastBalanceReferenceIsLeft))
    {
        balanceReferencePublished = true;
        lastBalanceReferenceIsLeft = decoded->pedalPowerBalanceReferenceIsLeft;
        objPtr.setProtectedPropertyValue(String("PedalPowerBalanceReference"),
                                         String(lastBalanceReferenceIsLeft ? "Left" : "Unknown"));
    }

    if (decoded->accumulatedTorqueNm &&
        (!torqueSourcePublished || decoded->accumulatedTorqueIsCrankBased != lastTorqueIsCrankBased))
    {
        torqueSourcePublished = true;
        lastTorqueIsCrankBased = decoded->accumulatedTorqueIsCrankBased;
        objPtr.setProtectedPropertyValue(String("AccumulatedTorqueSource"),
                                         String(lastTorqueIsCrankBased ? "Crank Based" : "Wheel Based"));
    }

    power.send(arrival, decoded->instantaneousPowerWatts);

    if (decoded->pedalPowerBalancePercent)
        pedalBalance.send(arrival, *decoded->pedalPowerBalancePercent);

    if (decoded->accumulatedTorqueNm)
        torque.send(arrival, *decoded->accumulatedTorqueNm);

    if (decoded->accumulatedEnergyKj)
        energy.send(arrival, *decoded->accumulatedEnergyKj);

    if (decoded->maximumForceNewtons)
        maximumForce.send(arrival, *decoded->maximumForceNewtons);
    if (decoded->minimumForceNewtons)
        minimumForce.send(arrival, *decoded->minimumForceNewtons);
    if (decoded->maximumTorqueNm)
        maximumTorque.send(arrival, *decoded->maximumTorqueNm);
    if (decoded->minimumTorqueNm)
        minimumTorque.send(arrival, *decoded->minimumTorqueNm);
    if (decoded->maximumAngleDegrees)
        maximumAngle.send(arrival, *decoded->maximumAngleDegrees);
    if (decoded->minimumAngleDegrees)
        minimumAngle.send(arrival, *decoded->minimumAngleDegrees);
    if (decoded->topDeadSpotAngleDegrees)
        topDeadSpotAngle.send(arrival, *decoded->topDeadSpotAngleDegrees);
    if (decoded->bottomDeadSpotAngleDegrees)
        bottomDeadSpotAngle.send(arrival, *decoded->bottomDeadSpotAngleDegrees);

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

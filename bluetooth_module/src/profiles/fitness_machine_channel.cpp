#include <bluetooth_module/ble/ble_types.h>
#include <bluetooth_module/profiles/fitness_machine_channel.h>

#include <coreobjects/argument_info_factory.h>
#include <coreobjects/callable_info_factory.h>
#include <coreobjects/property_object_protected_ptr.h>
#include <coreobjects/unit_factory.h>
#include <opendaq/custom_log.h>
#include <opendaq/signal_factory.h>

#include <cstdint>
#include <memory>
#include <string>
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
 * Expected spacing between samples, in ticks. Indoor Bike Data is usually 1 Hz but some trainers
 * notify at 4 Hz, and a machine that has been stopped goes quiet altogether.
 */
constexpr int64_t MinExpectedDeltaTicks = TicksPerSecond / 50;   // 20 ms
constexpr int64_t MaxExpectedDeltaTicks = TicksPerSecond * 30;   // 30 s

}

const char* FitnessMachineChannel::serviceUuid()
{
    return "00001826-0000-1000-8000-00805f9b34fb";
}

const char* FitnessMachineChannel::indoorBikeDataCharacteristicUuid()
{
    return "00002ad2-0000-1000-8000-00805f9b34fb";
}

const char* FitnessMachineChannel::featureCharacteristicUuid()
{
    return "00002acc-0000-1000-8000-00805f9b34fb";
}

const char* FitnessMachineChannel::trainingStatusCharacteristicUuid()
{
    return "00002ad3-0000-1000-8000-00805f9b34fb";
}

const char* FitnessMachineChannel::supportedResistanceLevelRangeCharacteristicUuid()
{
    return "00002ad6-0000-1000-8000-00805f9b34fb";
}

const char* FitnessMachineChannel::supportedPowerRangeCharacteristicUuid()
{
    return "00002ad8-0000-1000-8000-00805f9b34fb";
}

const char* FitnessMachineChannel::controlPointCharacteristicUuid()
{
    return "00002ad9-0000-1000-8000-00805f9b34fb";
}

FitnessMachineChannel::FitnessMachineChannel(const ContextPtr& context,
                                             const ComponentPtr& parent,
                                             const StringPtr& localId,
                                             std::shared_ptr<ble::IConnection> connection,
                                             const ble::Service& service)
    : ChannelImpl(FunctionBlockType("BluetoothFitnessMachine",
                                    "Fitness Machine",
                                    "Bluetooth SIG Fitness Machine service (indoor bike)"),
                  context,
                  parent,
                  localId)
    , connection(std::move(connection))
    , epochOffset(profile_support::currentEpochOffset())
    , steadyStart(steady_clock::now())
{
    // A treadmill, rower or cross trainer hosts this same service with its own data
    // characteristic. Declining here is how they are politely skipped.
    const auto* indoorBikeData =
        profile_support::characteristicOf(service, indoorBikeDataCharacteristicUuid());
    if (indoorBikeData == nullptr)
        DAQ_THROW_EXCEPTION(NotFoundException,
                            "The Fitness Machine service does not expose an Indoor Bike Data "
                            "characteristic; only indoor bikes are modelled.");

    if ((indoorBikeData->properties & (ble::CharPropNotify | ble::CharPropIndicate)) == 0)
        DAQ_THROW_EXCEPTION(InvalidStateException,
                            "The Indoor Bike Data characteristic does not support notifications.");

    indoorBikeDataHandle = indoorBikeData->handle;
    controlPointHandle = profile_support::handleOf(service, controlPointCharacteristicUuid());

    initProperties(service);
    createSignals();

    subscribeTo(indoorBikeDataHandle,
                [this](const std::vector<uint8_t>& payload) { onIndoorBikeData(payload); });

    // The control point answers by indication, so its verdict is only visible to a subscriber.
    if (!controlPointHandle.empty())
    {
        subscribeTo(controlPointHandle,
                    [this](const std::vector<uint8_t>& payload) { onControlPointResponse(payload); });
    }

    const auto trainingStatusHandle =
        profile_support::handleOf(service, trainingStatusCharacteristicUuid());
    if (!trainingStatusHandle.empty())
    {
        subscribeTo(trainingStatusHandle,
                    [this](const std::vector<uint8_t>& payload) { onTrainingStatus(payload); });
    }
}

FitnessMachineChannel::~FitnessMachineChannel()
{
    // First act, before anything is released: a control procedure held by an application, or a
    // notification already in flight on a backend thread, must stop touching this object now.
    shuttingDown = true;

    // Each handle released separately. A single try around the loop would let a throw on the
    // first leave the trainer notifying into a dead object through the rest.
    for (const auto& handle : subscribedHandles)
    {
        try
        {
            connection->unsubscribe(handle);
        }
        catch (...)
        {
            // The link may already be down; nothing useful to do about it during teardown.
            // Deliberately catch everything: a WinRT hresult_error does not derive from
            // std::exception, and an exception escaping a destructor is std::terminate().
        }
    }
}

void FitnessMachineChannel::subscribeTo(const std::string& handle,
                                        const std::function<void(const std::vector<uint8_t>&)>& handler)
{
    connection->subscribe(handle, handler);

    // Recorded only once the subscription took, so a failed one is not unsubscribed on teardown.
    subscribedHandles.push_back(handle);
}

void FitnessMachineChannel::initProperties(const ble::Service& service)
{
    const auto objPtr = this->template borrowPtr<PropertyObjectPtr>();

    objPtr.addProperty(StringPropertyBuilder("Features", "Unknown").setReadOnly(True).build());
    objPtr.addProperty(
        StringPropertyBuilder("TargetSettingFeatures", "Unknown").setReadOnly(True).build());

    const auto featureHandle = profile_support::handleOf(service, featureCharacteristicUuid());
    if (!featureHandle.empty())
    {
        readFeature(featureHandle);
    }

    const auto powerRangeHandle =
        profile_support::handleOf(service, supportedPowerRangeCharacteristicUuid());
    if (!powerRangeHandle.empty())
    {
        readPowerRange(powerRangeHandle);
    }

    const auto resistanceRangeHandle =
        profile_support::handleOf(service, supportedResistanceLevelRangeCharacteristicUuid());
    if (!resistanceRangeHandle.empty())
    {
        readResistanceRange(resistanceRangeHandle);
    }

    const auto trainingStatusHandle =
        profile_support::handleOf(service, trainingStatusCharacteristicUuid());
    if (!trainingStatusHandle.empty())
    {
        objPtr.addProperty(
            StringPropertyBuilder("TrainingStatus", "Unknown").setReadOnly(True).build());
        readTrainingStatus(trainingStatusHandle);
    }

    addControlActions();
}

void FitnessMachineChannel::readFeature(const std::string& handle)
{
    try
    {
        const auto decoded = profiles::decodeFitnessMachineFeature(connection->read(handle));
        if (!decoded)
            return;

        features = *decoded;

        const auto objPtr = this->template borrowPtr<PropertyObjectProtectedPtr>();
        objPtr.setProtectedPropertyValue(
            String("Features"),
            String(profiles::describeFitnessMachineFeatures(features.machineFeatures)));
        objPtr.setProtectedPropertyValue(
            String("TargetSettingFeatures"),
            String(profiles::describeTargetSettingFeatures(features.targetSettingFeatures)));
    }
    catch (const std::exception& e)
    {
        // Without the feature word no control action is offered at all, so say so plainly.
        LOG_W("Cannot read the fitness machine feature set, so no control actions will be "
              "available: {}",
              e.what())
    }
}

void FitnessMachineChannel::readPowerRange(const std::string& handle)
{
    try
    {
        const auto decoded = profiles::decodeSupportedPowerRange(connection->read(handle));
        if (!decoded)
            return;

        powerRange = *decoded;
        powerRangeKnown = true;

        const auto objPtr = this->template borrowPtr<PropertyObjectPtr>();
        objPtr.addProperty(
            IntPropertyBuilder("MinTargetPowerW", static_cast<Int>(powerRange.minimum))
                .setReadOnly(True)
                .build());
        objPtr.addProperty(
            IntPropertyBuilder("MaxTargetPowerW", static_cast<Int>(powerRange.maximum))
                .setReadOnly(True)
                .build());
        objPtr.addProperty(
            IntPropertyBuilder("TargetPowerIncrementW", static_cast<Int>(powerRange.increment))
                .setReadOnly(True)
                .build());
    }
    catch (const std::exception& e)
    {
        LOG_D("Cannot read the supported power range: {}", e.what())
    }
}

void FitnessMachineChannel::readResistanceRange(const std::string& handle)
{
    try
    {
        const auto decoded = profiles::decodeSupportedResistanceLevelRange(connection->read(handle));
        if (!decoded)
            return;

        resistanceRange = *decoded;
        resistanceRangeKnown = true;

        const auto objPtr = this->template borrowPtr<PropertyObjectPtr>();
        objPtr.addProperty(FloatPropertyBuilder("MinTargetResistanceLevel", resistanceRange.minimum)
                               .setReadOnly(True)
                               .build());
        objPtr.addProperty(FloatPropertyBuilder("MaxTargetResistanceLevel", resistanceRange.maximum)
                               .setReadOnly(True)
                               .build());
        objPtr.addProperty(
            FloatPropertyBuilder("TargetResistanceLevelIncrement", resistanceRange.increment)
                .setReadOnly(True)
                .build());

        // The control point takes a resistance target as a uint8 in tenths, so it cannot express
        // more than 25.5. A trainer reporting a larger maximum is treating the octet as a whole
        // level, and SetTargetResistanceLevel(7.5) will land on level 75 there. Nothing can be
        // done about it in-band -- but a log line beats silent nonsense.
        if (resistanceRange.maximum > 25.5)
        {
            LOG_I("This machine reports a maximum resistance level of {}, which the control "
                  "point's tenths encoding cannot express; resistance targets may be scaled "
                  "differently than expected",
                  resistanceRange.maximum)
        }
    }
    catch (const std::exception& e)
    {
        LOG_D("Cannot read the supported resistance level range: {}", e.what())
    }
}

void FitnessMachineChannel::readTrainingStatus(const std::string& handle)
{
    try
    {
        const auto status = profiles::decodeTrainingStatus(connection->read(handle));
        if (!status)
            return;

        lastTrainingStatus = *status;
        this->template borrowPtr<PropertyObjectProtectedPtr>().setProtectedPropertyValue(
            String("TrainingStatus"), String(profiles::trainingStatusName(*status)));
    }
    catch (const std::exception& e)
    {
        LOG_D("Cannot read the training status: {}", e.what())
    }
}

void FitnessMachineChannel::addControlActions()
{
    if (controlPointHandle.empty())
    {
        LOG_I("This fitness machine exposes no control point, so it can be read but not driven")
        return;
    }

    const auto objPtr = this->template borrowPtr<PropertyObjectPtr>();
    const auto protectedPtr = objPtr.template asPtr<IPropertyObjectProtected>();

    objPtr.addProperty(
        StringPropertyBuilder("LastControlResult", "None").setReadOnly(True).build());

    // Function/procedure properties are protected in some openDAQ builds, where the public
    // setPropertyValue() rejects the callable. Use the protected setter, as the rest of the
    // module does, so this builds against both.
    const auto addAction = [&](const char* name, const ListPtr<IArgumentInfo>& arguments) {
        objPtr.addProperty(FunctionProperty(name, ProcedureInfo(arguments)));
    };

    // Every lambda below captures a raw `this` on purpose. These callables are property values of
    // the very object they act on, so capturing a smart pointer would make the channel hold a
    // reference to itself -- a cycle no destructor can break.
    addAction("RequestControl", List<IArgumentInfo>());
    protectedPtr.setProtectedPropertyValue(
        String("RequestControl"),
        Procedure([this] { writeControlPoint(profiles::encodeRequestControl()); }));

    addAction("Reset", List<IArgumentInfo>());
    protectedPtr.setProtectedPropertyValue(
        String("Reset"), Procedure([this] { writeControlPoint(profiles::encodeReset()); }));

    addAction("Start", List<IArgumentInfo>());
    protectedPtr.setProtectedPropertyValue(
        String("Start"), Procedure([this] { writeControlPoint(profiles::encodeStartOrResume()); }));

    addAction("Stop", List<IArgumentInfo>());
    protectedPtr.setProtectedPropertyValue(
        String("Stop"), Procedure([this] { writeControlPoint(profiles::encodeStop()); }));

    addAction("Pause", List<IArgumentInfo>());
    protectedPtr.setProtectedPropertyValue(
        String("Pause"), Procedure([this] { writeControlPoint(profiles::encodePause()); }));

    // The remaining actions exist only where the trainer says it accepts them. An always-present
    // button that always fails would be worse than no button.
    if ((features.targetSettingFeatures & profiles::TargetSettingPower) != 0)
    {
        addAction("SetTargetPower", List<IArgumentInfo>(ArgumentInfo("Watts", ctInt)));
        protectedPtr.setProtectedPropertyValue(
            String("SetTargetPower"), Procedure([this](const IntegerPtr& watts) {
                const auto target = static_cast<double>(static_cast<Int>(watts));

                // Rejected here rather than at the trainer: the caller gets a specific error now
                // instead of an asynchronous "Invalid Parameter" several hundred milliseconds
                // later, with no way to tell which request it belonged to.
                if (powerRangeKnown && (target < powerRange.minimum || target > powerRange.maximum))
                    DAQ_THROW_EXCEPTION(InvalidParameterException,
                                        "Target power is outside the range this machine supports.");

                writeControlPoint(profiles::encodeSetTargetPower(target));
            }));
    }

    if ((features.targetSettingFeatures & profiles::TargetSettingResistance) != 0)
    {
        addAction("SetTargetResistanceLevel", List<IArgumentInfo>(ArgumentInfo("Level", ctFloat)));
        protectedPtr.setProtectedPropertyValue(
            String("SetTargetResistanceLevel"), Procedure([this](const FloatPtr& level) {
                const auto target = static_cast<double>(static_cast<Float>(level));

                if (resistanceRangeKnown &&
                    (target < resistanceRange.minimum || target > resistanceRange.maximum))
                    DAQ_THROW_EXCEPTION(
                        InvalidParameterException,
                        "Target resistance level is outside the range this machine supports.");

                writeControlPoint(profiles::encodeSetTargetResistanceLevel(target));
            }));
    }

    if ((features.targetSettingFeatures & profiles::TargetSettingIndoorBikeSimulation) != 0)
    {
        addAction("SetSimulationParameters",
                  List<IArgumentInfo>(ArgumentInfo("WindSpeedMps", ctFloat),
                                      ArgumentInfo("GradePercent", ctFloat),
                                      ArgumentInfo("RollingResistanceCoefficient", ctFloat),
                                      ArgumentInfo("WindResistanceCoefficient", ctFloat)));
        protectedPtr.setProtectedPropertyValue(
            String("SetSimulationParameters"),
            Procedure([this](const FloatPtr& windSpeed,
                             const FloatPtr& grade,
                             const FloatPtr& rollingResistance,
                             const FloatPtr& windResistance) {
                writeControlPoint(profiles::encodeSetSimulationParameters(
                    static_cast<Float>(windSpeed),
                    static_cast<Float>(grade),
                    static_cast<Float>(rollingResistance),
                    static_cast<Float>(windResistance)));
            }));
    }
}

void FitnessMachineChannel::writeControlPoint(const std::vector<uint8_t>& request)
{
    // An application can hold a ProcedurePtr taken from a property after dropping every reference
    // to the channel. Best effort -- the memory is already gone by then -- but it closes the wide
    // window rather than the narrow one.
    if (shuttingDown)
        DAQ_THROW_EXCEPTION(InvalidStateException, "This fitness machine channel is shutting down.");

    // Deliberately NOT holding `sync` across this call. A write with response blocks until the
    // trainer answers, and on Linux that answer arrives on the very thread that delivers this
    // characteristic's indications -- which take `sync`. A trainer that indicated before its write
    // response landed would deadlock the two against each other. Nothing this touches is mutable
    // after construction, so there is nothing here for the lock to protect anyway.
    try
    {
        connection->write(controlPointHandle, request, true);
    }
    catch (const std::exception& e)
    {
        LOG_W("Cannot write the fitness machine control point: {}", e.what())
        DAQ_THROW_EXCEPTION(InvalidStateException, e.what());
    }
}

void FitnessMachineChannel::createSignals()
{
    // One domain signal per value signal. The Fitness Machine service explicitly allows one data
    // record to be split across several notifications, so no two of these fields can be assumed
    // to arrive together -- and signals sharing a domain must produce equal sample counts.
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

    const auto kph = Unit("km/h", -1, "kilometres per hour", "velocity");
    const auto rpm = Unit("rpm", -1, "revolutions per minute", "angular velocity");
    const auto watts = Unit("W", -1, "watt", "power");
    const auto kcal = Unit("kcal", -1, "kilocalorie", "energy");
    const auto seconds = Unit("s", -1, "second", "time");

    speed = pair("Speed", "Instantaneous Speed", kph, Range(0, 150));
    averageSpeed = pair("AverageSpeed", "Average Speed", kph, Range(0, 150));
    cadence = pair("Cadence", "Instantaneous Cadence", rpm, Range(0, 250));
    averageCadence = pair("AverageCadence", "Average Cadence", rpm, Range(0, 250));
    totalDistance = pair("TotalDistance", "Total Distance", Unit("m", -1, "metre", "length"), nullptr);
    resistanceLevel = pair("ResistanceLevel", "Resistance Level",
                           Unit("", -1, "resistance level", "unitless"), nullptr);
    power = pair("InstantaneousPower", "Instantaneous Power", watts, Range(-2000, 3000));
    averagePower = pair("AveragePower", "Average Power", watts, Range(-2000, 3000));
    totalEnergy = pair("TotalEnergy", "Total Energy", kcal, nullptr);
    energyPerHour = pair("EnergyPerHour", "Energy Per Hour",
                         Unit("kcal/h", -1, "kilocalories per hour", "power"), nullptr);
    energyPerMinute = pair("EnergyPerMinute", "Energy Per Minute",
                           Unit("kcal/min", -1, "kilocalories per minute", "power"), nullptr);
    heartRate = pair("HeartRate", "Heart Rate",
                     Unit("bpm", -1, "beats per minute", "heart rate"), Range(0, 300));
    metabolicEquivalent = pair("MetabolicEquivalent", "Metabolic Equivalent",
                               Unit("MET", -1, "metabolic equivalent", "unitless"), nullptr);
    elapsedTime = pair("ElapsedTime", "Elapsed Time", seconds, nullptr);
    remainingTime = pair("RemainingTime", "Remaining Time", seconds, nullptr);
}

int64_t FitnessMachineChannel::nowTicks() const
{
    const auto elapsed = duration_cast<microseconds>(steady_clock::now() - steadyStart);
    return (epochOffset + elapsed).count();
}

void FitnessMachineChannel::onIndoorBikeData(const std::vector<uint8_t>& payload)
{
    const auto decoded = profiles::decodeIndoorBikeData(payload);
    if (!decoded)
    {
        LOG_W("Discarding a malformed Indoor Bike Data notification of {} byte(s)", payload.size())
        return;
    }

    const auto arrival = nowTicks();

    std::scoped_lock lock(sync);

    if (shuttingDown)
        return;

    const auto emit = [arrival](const profile_support::SignalPair& signal, const auto& value) {
        if (value)
            signal.send(arrival, static_cast<double>(*value));
    };

    emit(speed, decoded->instantaneousSpeedKph);
    emit(averageSpeed, decoded->averageSpeedKph);
    emit(cadence, decoded->instantaneousCadenceRpm);
    emit(averageCadence, decoded->averageCadenceRpm);
    emit(totalDistance, decoded->totalDistanceMetres);
    emit(resistanceLevel, decoded->resistanceLevel);
    emit(power, decoded->instantaneousPowerWatts);
    emit(averagePower, decoded->averagePowerWatts);
    emit(totalEnergy, decoded->totalEnergyKcal);
    emit(energyPerHour, decoded->energyPerHourKcal);
    emit(energyPerMinute, decoded->energyPerMinuteKcal);
    emit(heartRate, decoded->heartRateBpm);
    emit(metabolicEquivalent, decoded->metabolicEquivalent);
    emit(elapsedTime, decoded->elapsedTimeSeconds);
    emit(remainingTime, decoded->remainingTimeSeconds);
}

void FitnessMachineChannel::onControlPointResponse(const std::vector<uint8_t>& payload)
{
    const auto response = profiles::decodeControlPointResponse(payload);
    if (!response)
        return;

    std::scoped_lock lock(sync);

    if (shuttingDown)
        return;

    // Recorded, never acted on. This callback runs on a backend thread that on Linux holds the
    // connection's D-Bus mutex, so reading or writing a characteristic from here -- to re-request
    // control after a "Control Not Permitted", say -- would deadlock against the very thread
    // delivering the answer. Surface it and let the application decide.
    const auto summary = std::string(profiles::controlPointOpCodeName(response->requestOpCode)) +
                         ": " + profiles::controlPointResultName(response->result);

    this->template borrowPtr<PropertyObjectProtectedPtr>().setProtectedPropertyValue(
        String("LastControlResult"), String(summary));
}

void FitnessMachineChannel::onTrainingStatus(const std::vector<uint8_t>& payload)
{
    const auto status = profiles::decodeTrainingStatus(payload);
    if (!status)
        return;

    std::scoped_lock lock(sync);

    if (shuttingDown)
        return;

    // Only on a real change: setting a property notifies every listener synchronously, on the
    // backend thread, and a machine repeats its status for the whole of a workout.
    if (*status == lastTrainingStatus)
        return;

    lastTrainingStatus = *status;

    this->template borrowPtr<PropertyObjectProtectedPtr>().setProtectedPropertyValue(
        String("TrainingStatus"), String(profiles::trainingStatusName(*status)));
}

END_NAMESPACE_OPENDAQ_BLUETOOTH

#include <bluetooth_module/profiles/byte_reader.h>
#include <bluetooth_module/profiles/cycling_common.h>
#include <bluetooth_module/profiles/indoor_bike_data.h>

#include <algorithm>
#include <cmath>

namespace opendaq_bt::profiles
{

namespace
{

// Flag bits of the 16-bit flags field opening Indoor Bike Data (0x2AD2).
//
// Bit 0 is the odd one out and the single easiest field in this module to get wrong: it is called
// "More Data", and the Instantaneous Speed field is present when it is CLEAR, not when it is set.
// Reading it the intuitive way shifts every subsequent field by two octets, which yields entirely
// plausible numbers for the wrong quantities.
constexpr uint16_t FlagMoreData = 1u << 0;
constexpr uint16_t FlagAverageSpeed = 1u << 1;
constexpr uint16_t FlagInstantaneousCadence = 1u << 2;
constexpr uint16_t FlagAverageCadence = 1u << 3;
constexpr uint16_t FlagTotalDistance = 1u << 4;
constexpr uint16_t FlagResistanceLevel = 1u << 5;
constexpr uint16_t FlagInstantaneousPower = 1u << 6;
constexpr uint16_t FlagAveragePower = 1u << 7;
constexpr uint16_t FlagExpendedEnergy = 1u << 8;
constexpr uint16_t FlagHeartRate = 1u << 9;
constexpr uint16_t FlagMetabolicEquivalent = 1u << 10;
constexpr uint16_t FlagElapsedTime = 1u << 11;
constexpr uint16_t FlagRemainingTime = 1u << 12;

constexpr double SpeedResolution = 0.01;             // km/h
constexpr double CadenceResolution = 0.5;            // 1/min
constexpr double MetabolicEquivalentResolution = 0.1;

/*! Machines that cannot supply an energy figure send an all-ones field rather than omitting it. */
constexpr uint16_t EnergyUnavailable16 = 0xFFFF;
constexpr uint8_t EnergyUnavailable8 = 0xFF;

/*! Fitness Machine Control Point (0x2AD9) opcodes. */
constexpr uint8_t OpRequestControl = 0x00;
constexpr uint8_t OpReset = 0x01;
constexpr uint8_t OpSetTargetResistanceLevel = 0x04;
constexpr uint8_t OpSetTargetPower = 0x05;
constexpr uint8_t OpStartOrResume = 0x07;
constexpr uint8_t OpStopOrPause = 0x08;
constexpr uint8_t OpSetSimulationParameters = 0x11;
constexpr uint8_t OpResponseCode = 0x80;

constexpr uint8_t StopOrPauseStop = 0x01;
constexpr uint8_t StopOrPausePause = 0x02;

const char* const MachineFeatureNames[] = {
    "Average Speed",
    "Cadence",
    "Total Distance",
    "Inclination",
    "Elevation Gain",
    "Pace",
    "Step Count",
    "Resistance Level",
    "Stride Count",
    "Expended Energy",
    "Heart Rate Measurement",
    "Metabolic Equivalent",
    "Elapsed Time",
    "Remaining Time",
    "Power Measurement",
    "Force on Belt and Power Output",
    "User Data Retention",
};

const char* const TargetSettingFeatureNames[] = {
    "Speed Target",
    "Inclination Target",
    "Resistance Target",
    "Power Target",
    "Heart Rate Target",
    "Expended Energy Configuration",
    "Step Number Configuration",
    "Stride Number Configuration",
    "Distance Configuration",
    "Training Time Configuration",
    "Time in Two Heart Rate Zones",
    "Time in Three Heart Rate Zones",
    "Time in Five Heart Rate Zones",
    "Indoor Bike Simulation",
    "Wheel Circumference Configuration",
    "Spin Down Control",
    "Cadence Configuration",
};

/*! Rounds and clamps, so a caller passing an absurd target writes the nearest legal value. */
int64_t quantise(double value, double resolution, int64_t low, int64_t high)
{
    const auto scaled = std::llround(value / resolution);
    return std::clamp<int64_t>(scaled, low, high);
}

void appendUint8(std::vector<uint8_t>& out, int64_t value)
{
    out.push_back(static_cast<uint8_t>(value));
}

void appendInt16(std::vector<uint8_t>& out, int64_t value)
{
    const auto raw = static_cast<uint16_t>(static_cast<int16_t>(value));
    out.push_back(static_cast<uint8_t>(raw & 0xFF));
    out.push_back(static_cast<uint8_t>((raw >> 8) & 0xFF));
}

}

std::optional<IndoorBikeData> decodeIndoorBikeData(const std::vector<uint8_t>& payload)
{
    ByteReader reader(payload);

    const auto flags = reader.u16();

    IndoorBikeData data;

    if ((flags & FlagMoreData) == 0)
        data.instantaneousSpeedKph = reader.u16() * SpeedResolution;

    if ((flags & FlagAverageSpeed) != 0)
        data.averageSpeedKph = reader.u16() * SpeedResolution;

    if ((flags & FlagInstantaneousCadence) != 0)
        data.instantaneousCadenceRpm = reader.u16() * CadenceResolution;

    if ((flags & FlagAverageCadence) != 0)
        data.averageCadenceRpm = reader.u16() * CadenceResolution;

    if ((flags & FlagTotalDistance) != 0)
        data.totalDistanceMetres = reader.u24();

    if ((flags & FlagResistanceLevel) != 0)
        data.resistanceLevel = reader.s16();

    if ((flags & FlagInstantaneousPower) != 0)
        data.instantaneousPowerWatts = reader.s16();

    if ((flags & FlagAveragePower) != 0)
        data.averagePowerWatts = reader.s16();

    if ((flags & FlagExpendedEnergy) != 0)
    {
        const auto total = reader.u16();
        const auto perHour = reader.u16();
        const auto perMinute = reader.u8();

        // The all-ones patterns mean "this machine cannot tell you", not 65535 kcal.
        if (total != EnergyUnavailable16)
            data.totalEnergyKcal = total;
        if (perHour != EnergyUnavailable16)
            data.energyPerHourKcal = perHour;
        if (perMinute != EnergyUnavailable8)
            data.energyPerMinuteKcal = perMinute;
    }

    if ((flags & FlagHeartRate) != 0)
        data.heartRateBpm = reader.u8();

    if ((flags & FlagMetabolicEquivalent) != 0)
        data.metabolicEquivalent = reader.u8() * MetabolicEquivalentResolution;

    if ((flags & FlagElapsedTime) != 0)
        data.elapsedTimeSeconds = reader.u16();

    if ((flags & FlagRemainingTime) != 0)
        data.remainingTimeSeconds = reader.u16();

    if (reader.overran())
        return std::nullopt;

    return data;
}

std::optional<FitnessMachineFeature> decodeFitnessMachineFeature(const std::vector<uint8_t>& payload)
{
    ByteReader reader(payload);

    FitnessMachineFeature feature;
    feature.machineFeatures = reader.u32();
    feature.targetSettingFeatures = reader.u32();

    if (reader.overran())
        return std::nullopt;

    return feature;
}

std::string describeFitnessMachineFeatures(uint32_t features)
{
    return describeBits(features, MachineFeatureNames,
                        sizeof(MachineFeatureNames) / sizeof(MachineFeatureNames[0]));
}

std::string describeTargetSettingFeatures(uint32_t features)
{
    return describeBits(features, TargetSettingFeatureNames,
                        sizeof(TargetSettingFeatureNames) / sizeof(TargetSettingFeatureNames[0]));
}

std::optional<SupportedRange> decodeSupportedPowerRange(const std::vector<uint8_t>& payload)
{
    ByteReader reader(payload);

    SupportedRange range;
    range.minimum = reader.s16();
    range.maximum = reader.s16();
    range.increment = reader.u16();

    if (reader.overran())
        return std::nullopt;

    return range;
}

std::optional<SupportedRange> decodeSupportedResistanceLevelRange(const std::vector<uint8_t>& payload)
{
    // Same three fields as the power range, but in tenths. Note the asymmetry with the control
    // point, which takes a resistance target as a uint8 in tenths -- that is the specification's
    // inconsistency, not a mistake here.
    constexpr double ResistanceResolution = 0.1;

    ByteReader reader(payload);

    SupportedRange range;
    range.minimum = reader.s16() * ResistanceResolution;
    range.maximum = reader.s16() * ResistanceResolution;
    range.increment = reader.u16() * ResistanceResolution;

    if (reader.overran())
        return std::nullopt;

    return range;
}

const char* trainingStatusName(uint8_t value)
{
    switch (value)
    {
        case 0x00: return "Other";
        case 0x01: return "Idle";
        case 0x02: return "Warming Up";
        case 0x03: return "Low Intensity Interval";
        case 0x04: return "High Intensity Interval";
        case 0x05: return "Recovery Interval";
        case 0x06: return "Isometric";
        case 0x07: return "Heart Rate Control";
        case 0x08: return "Fitness Test";
        case 0x09: return "Speed Outside of Control Region - Low";
        case 0x0A: return "Speed Outside of Control Region - High";
        case 0x0B: return "Cool Down";
        case 0x0C: return "Watt Control";
        case 0x0D: return "Manual Mode";
        case 0x0E: return "Pre-Workout";
        case 0x0F: return "Post-Workout";
        default: return "Unknown";
    }
}

std::optional<uint8_t> decodeTrainingStatus(const std::vector<uint8_t>& payload)
{
    ByteReader reader(payload);

    reader.u8();   // Flags: whether human-readable status strings follow. Not used here.
    const auto status = reader.u8();

    if (reader.overran())
        return std::nullopt;

    return status;
}

std::optional<ControlPointResponse> decodeControlPointResponse(const std::vector<uint8_t>& payload)
{
    ByteReader reader(payload);

    // A control point indication that is not a response to a request is not something this module
    // asked for, and reading it as one would attribute a stray result to the last command sent.
    if (reader.u8() != OpResponseCode)
        return std::nullopt;

    ControlPointResponse response;
    response.requestOpCode = reader.u8();
    response.result = reader.u8();

    if (reader.overran())
        return std::nullopt;

    return response;
}

const char* controlPointOpCodeName(uint8_t opCode)
{
    switch (opCode)
    {
        case OpRequestControl: return "Request Control";
        case OpReset: return "Reset";
        case OpSetTargetResistanceLevel: return "Set Target Resistance Level";
        case OpSetTargetPower: return "Set Target Power";
        case OpStartOrResume: return "Start or Resume";
        case OpStopOrPause: return "Stop or Pause";
        case OpSetSimulationParameters: return "Set Simulation Parameters";
        default: return "Unknown";
    }
}

const char* controlPointResultName(uint8_t result)
{
    switch (result)
    {
        case 0x01: return "Success";
        case 0x02: return "Op Code Not Supported";
        case 0x03: return "Invalid Parameter";
        case 0x04: return "Operation Failed";
        case 0x05: return "Control Not Permitted";
        default: return "Unknown";
    }
}

std::vector<uint8_t> encodeRequestControl()
{
    return {OpRequestControl};
}

std::vector<uint8_t> encodeReset()
{
    return {OpReset};
}

std::vector<uint8_t> encodeStartOrResume()
{
    return {OpStartOrResume};
}

std::vector<uint8_t> encodeStop()
{
    return {OpStopOrPause, StopOrPauseStop};
}

std::vector<uint8_t> encodePause()
{
    return {OpStopOrPause, StopOrPausePause};
}

std::vector<uint8_t> encodeSetTargetPower(double watts)
{
    std::vector<uint8_t> request{OpSetTargetPower};
    appendInt16(request, quantise(watts, 1.0, -32768, 32767));
    return request;
}

std::vector<uint8_t> encodeSetTargetResistanceLevel(double level)
{
    std::vector<uint8_t> request{OpSetTargetResistanceLevel};
    appendUint8(request, quantise(level, 0.1, 0, 255));
    return request;
}

std::vector<uint8_t> encodeSetSimulationParameters(double windSpeedMps,
                                                   double gradePercent,
                                                   double crr,
                                                   double cw)
{
    std::vector<uint8_t> request{OpSetSimulationParameters};

    appendInt16(request, quantise(windSpeedMps, 0.001, -32768, 32767));
    appendInt16(request, quantise(gradePercent, 0.01, -32768, 32767));
    appendUint8(request, quantise(crr, 0.0001, 0, 255));
    appendUint8(request, quantise(cw, 0.01, 0, 255));

    return request;
}

}

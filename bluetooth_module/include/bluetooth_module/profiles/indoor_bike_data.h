#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opendaq_bt::profiles
{

/*!
 * One decoded Indoor Bike Data notification (Bluetooth SIG characteristic 0x2AD2).
 *
 * Every field is optional and announced by a bit of the 16-bit flags field -- including the speed,
 * whose bit is inverted (see the decoder). A trainer typically sends speed, cadence and power, and
 * a machine mid-workout adds elapsed time and energy.
 */
struct IndoorBikeData
{
    std::optional<double> instantaneousSpeedKph;
    std::optional<double> averageSpeedKph;

    std::optional<double> instantaneousCadenceRpm;
    std::optional<double> averageCadenceRpm;

    std::optional<uint32_t> totalDistanceMetres;

    /*! Machine-specific scale, not watts; Supported Resistance Level Range gives its bounds. */
    std::optional<int16_t> resistanceLevel;

    std::optional<int16_t> instantaneousPowerWatts;
    std::optional<int16_t> averagePowerWatts;

    /*! The three energy figures share one flag bit, so they arrive together or not at all. */
    std::optional<uint16_t> totalEnergyKcal;
    std::optional<uint16_t> energyPerHourKcal;
    std::optional<uint8_t> energyPerMinuteKcal;

    std::optional<uint8_t> heartRateBpm;
    std::optional<double> metabolicEquivalent;

    std::optional<uint16_t> elapsedTimeSeconds;
    std::optional<uint16_t> remainingTimeSeconds;
};

/*!
 * Decodes an Indoor Bike Data payload.
 *
 * Returns nullopt for a payload too short to hold its flags, or whose flags promise more than it
 * carries.
 */
std::optional<IndoorBikeData> decodeIndoorBikeData(const std::vector<uint8_t>& payload);

/*! The two words of a Fitness Machine Feature (0x2ACC) read. */
struct FitnessMachineFeature
{
    uint32_t machineFeatures = 0;

    /*! Which control point targets the machine accepts. Gates the actions this module offers. */
    uint32_t targetSettingFeatures = 0;
};

std::optional<FitnessMachineFeature> decodeFitnessMachineFeature(const std::vector<uint8_t>& payload);

/*! Bits of FitnessMachineFeature::targetSettingFeatures that gate an action this module exposes. */
constexpr uint32_t TargetSettingResistance = 1u << 2;
constexpr uint32_t TargetSettingPower = 1u << 3;
constexpr uint32_t TargetSettingIndoorBikeSimulation = 1u << 13;

std::string describeFitnessMachineFeatures(uint32_t features);
std::string describeTargetSettingFeatures(uint32_t features);

/*!
 * A Supported Power Range (0x2AD8) or Supported Resistance Level Range (0x2AD6) reading.
 *
 * Both have the same shape -- minimum, maximum, increment -- but not the same units: power is in
 * whole watts, resistance in tenths of a unit. The decoder for each applies its own scale.
 */
struct SupportedRange
{
    double minimum = 0.0;
    double maximum = 0.0;
    double increment = 0.0;
};

std::optional<SupportedRange> decodeSupportedPowerRange(const std::vector<uint8_t>& payload);
std::optional<SupportedRange> decodeSupportedResistanceLevelRange(const std::vector<uint8_t>& payload);

/*! Human-readable name for a Training Status (0x2AD3) enum value. */
const char* trainingStatusName(uint8_t value);

/*! Decodes the status octet out of a Training Status payload, past its flags octet. */
std::optional<uint8_t> decodeTrainingStatus(const std::vector<uint8_t>& payload);

/*! One decoded Fitness Machine Control Point (0x2AD9) indication. */
struct ControlPointResponse
{
    uint8_t requestOpCode = 0;
    uint8_t result = 0;
};

/*!
 * Decodes a control point indication.
 *
 * Returns nullopt unless the payload really is a response -- it must open with the 0x80 Response
 * Code opcode and carry the request opcode and result behind it.
 */
std::optional<ControlPointResponse> decodeControlPointResponse(const std::vector<uint8_t>& payload);

const char* controlPointOpCodeName(uint8_t opCode);
const char* controlPointResultName(uint8_t result);

// ---- control point requests ------------------------------------------------------------------
//
// Built here rather than in the channel so that the scaling -- which is per-opcode and easy to get
// wrong in a way no peripheral will complain about -- is testable without a radio.

std::vector<uint8_t> encodeRequestControl();
std::vector<uint8_t> encodeReset();
std::vector<uint8_t> encodeStartOrResume();
std::vector<uint8_t> encodeStop();
std::vector<uint8_t> encodePause();

/*! Watts, signed. Values outside the wire range are clamped rather than wrapped. */
std::vector<uint8_t> encodeSetTargetPower(double watts);

/*! Unitless machine scale, transmitted in tenths. Clamped to what a uint8 can carry. */
std::vector<uint8_t> encodeSetTargetResistanceLevel(double level);

/*!
 * Road simulation: what the trainer needs to work out the resistance for a virtual course.
 *
 * @param windSpeedMps  head wind, metres per second
 * @param gradePercent  slope, per cent, negative downhill
 * @param crr           coefficient of rolling resistance, around 0.004 on tarmac
 * @param cw            wind resistance coefficient, kg/m, around 0.51 for a rider on the hoods
 */
std::vector<uint8_t> encodeSetSimulationParameters(double windSpeedMps,
                                                   double gradePercent,
                                                   double crr,
                                                   double cw);

}

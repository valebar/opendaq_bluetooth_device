#include <bluetooth_module/profiles/byte_reader.h>
#include <bluetooth_module/profiles/cycling_power_measurement.h>

namespace opendaq_bt::profiles
{

namespace
{

// Flag bits of the 16-bit flags field opening Cycling Power Measurement (0x2A63), spelled out so
// the decoder reads as the specification does.
constexpr uint16_t FlagPedalPowerBalance = 1u << 0;
constexpr uint16_t FlagPedalPowerBalanceReferenceLeft = 1u << 1;
constexpr uint16_t FlagAccumulatedTorque = 1u << 2;
constexpr uint16_t FlagAccumulatedTorqueCrankBased = 1u << 3;
constexpr uint16_t FlagWheelRevolutionData = 1u << 4;
constexpr uint16_t FlagCrankRevolutionData = 1u << 5;
constexpr uint16_t FlagExtremeForceMagnitudes = 1u << 6;
constexpr uint16_t FlagExtremeTorqueMagnitudes = 1u << 7;
constexpr uint16_t FlagExtremeAngles = 1u << 8;
constexpr uint16_t FlagTopDeadSpotAngle = 1u << 9;
constexpr uint16_t FlagBottomDeadSpotAngle = 1u << 10;
constexpr uint16_t FlagAccumulatedEnergy = 1u << 11;
constexpr uint16_t FlagOffsetCompensationIndicator = 1u << 12;

/*! Pedal power balance is transmitted in halves of a percent. */
constexpr double PedalPowerBalanceResolution = 0.5;

/*! Torque, accumulated and extreme alike, is transmitted in thirty-seconds of a newton metre. */
constexpr double TorqueResolution = 1.0 / 32.0;

/*! The two extreme angles share three octets: maximum in the low 12 bits, minimum in the high 12. */
constexpr uint32_t AngleMask = 0xFFF;

const char* const FeatureNames[] = {
    "Pedal Power Balance",
    "Accumulated Torque",
    "Wheel Revolution Data",
    "Crank Revolution Data",
    "Extreme Magnitudes",
    "Extreme Angles",
    "Top and Bottom Dead Spot Angles",
    "Accumulated Energy",
    "Offset Compensation Indicator",
    "Offset Compensation",
    "Measurement Content Masking",
    "Multiple Sensor Locations",
    "Crank Length Adjustment",
    "Chain Length Adjustment",
    "Chain Weight Adjustment",
    "Span Length Adjustment",
    "Sensor Measurement Context",
    "Instantaneous Measurement Direction",
    "Factory Calibration Date",
    "Enhanced Offset Compensation",
};

}

std::optional<CyclingPowerMeasurement> decodeCyclingPowerMeasurement(const std::vector<uint8_t>& payload)
{
    ByteReader reader(payload);

    const auto flags = reader.u16();

    CyclingPowerMeasurement measurement;
    measurement.instantaneousPowerWatts = reader.s16();

    // The optional fields appear in flag-bit order, which is the only thing that makes a
    // variable-layout payload decodable at all. Reading them out of order shifts everything after.
    if ((flags & FlagPedalPowerBalance) != 0)
        measurement.pedalPowerBalancePercent = reader.u8() * PedalPowerBalanceResolution;

    measurement.pedalPowerBalanceReferenceIsLeft = (flags & FlagPedalPowerBalanceReferenceLeft) != 0;

    if ((flags & FlagAccumulatedTorque) != 0)
        measurement.accumulatedTorqueNm = reader.u16() * TorqueResolution;

    measurement.accumulatedTorqueIsCrankBased = (flags & FlagAccumulatedTorqueCrankBased) != 0;

    if ((flags & FlagWheelRevolutionData) != 0)
    {
        RevolutionData wheel;
        wheel.cumulativeRevolutions = reader.u32();
        wheel.lastEventTime = reader.u16();
        measurement.wheelRevolutions = wheel;
    }

    if ((flags & FlagCrankRevolutionData) != 0)
    {
        RevolutionData crank;
        crank.cumulativeRevolutions = reader.u16();
        crank.lastEventTime = reader.u16();
        measurement.crankRevolutions = crank;
    }

    if ((flags & FlagExtremeForceMagnitudes) != 0)
    {
        measurement.maximumForceNewtons = reader.s16();
        measurement.minimumForceNewtons = reader.s16();
    }

    if ((flags & FlagExtremeTorqueMagnitudes) != 0)
    {
        measurement.maximumTorqueNm = reader.s16() * TorqueResolution;
        measurement.minimumTorqueNm = reader.s16() * TorqueResolution;
    }

    if ((flags & FlagExtremeAngles) != 0)
    {
        const auto packed = reader.u24();
        measurement.maximumAngleDegrees = static_cast<uint16_t>(packed & AngleMask);
        measurement.minimumAngleDegrees = static_cast<uint16_t>((packed >> 12) & AngleMask);
    }

    if ((flags & FlagTopDeadSpotAngle) != 0)
        measurement.topDeadSpotAngleDegrees = reader.u16();

    if ((flags & FlagBottomDeadSpotAngle) != 0)
        measurement.bottomDeadSpotAngleDegrees = reader.u16();

    if ((flags & FlagAccumulatedEnergy) != 0)
        measurement.accumulatedEnergyKj = reader.u16();

    measurement.offsetCompensationIndicator = (flags & FlagOffsetCompensationIndicator) != 0;

    // One check for the whole frame: any field above that ran off the end poisoned the reader, and
    // the values it returned in the meantime are zeros rather than whatever followed in memory.
    if (reader.overran())
        return std::nullopt;

    return measurement;
}

std::string describeCyclingPowerFeatures(uint32_t features)
{
    return describeBits(features, FeatureNames, sizeof(FeatureNames) / sizeof(FeatureNames[0]));
}

}

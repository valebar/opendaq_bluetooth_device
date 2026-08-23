#include <bluetooth_module/profiles/byte_reader.h>
#include <bluetooth_module/profiles/cycling_speed_cadence_measurement.h>

namespace opendaq_bt::profiles
{

namespace
{

// Flag bits of the octet opening CSC Measurement (0x2A5B).
constexpr uint8_t FlagWheelRevolutionData = 1u << 0;
constexpr uint8_t FlagCrankRevolutionData = 1u << 1;

const char* const FeatureNames[] = {
    "Wheel Revolution Data",
    "Crank Revolution Data",
    "Multiple Sensor Locations",
};

}

std::optional<CscMeasurement> decodeCscMeasurement(const std::vector<uint8_t>& payload)
{
    ByteReader reader(payload);

    const auto flags = reader.u8();

    CscMeasurement measurement;

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

    if (reader.overran())
        return std::nullopt;

    return measurement;
}

std::string describeCscFeatures(uint16_t features)
{
    return describeBits(features, FeatureNames, sizeof(FeatureNames) / sizeof(FeatureNames[0]));
}

}

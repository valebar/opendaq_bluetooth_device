#include <bluetooth_module/profiles/byte_reader.h>
#include <bluetooth_module/profiles/temperature_measurement.h>

#include <cmath>

namespace opendaq_bt::profiles
{

namespace
{

// Flag bits of the octet opening Temperature Measurement (0x2A1C) and Intermediate Temperature.
constexpr uint8_t FlagFahrenheit = 1u << 0;
constexpr uint8_t FlagTimeStampPresent = 1u << 1;
constexpr uint8_t FlagTemperatureTypePresent = 1u << 2;

/*! ESS reports an unknown value as the most negative sint16. */
constexpr int16_t EnvironmentalTemperatureUnknown = INT16_MIN;

/*!
 * IEEE-11073 32-bit FLOAT: a signed 8-bit exponent over a signed 24-bit mantissa, base 10.
 * Returns nullopt for the reserved mantissas (NaN, NRes, +INF, -INF, reserved).
 */
std::optional<double> ieee11073Float(uint32_t raw)
{
    const uint32_t mantissaBits = raw & 0x00FFFFFFu;

    switch (mantissaBits)
    {
        case 0x007FFFFF:   // NaN
        case 0x00800000:   // NRes
        case 0x007FFFFE:   // +INF
        case 0x00800002:   // -INF
        case 0x00800001:   // reserved
            return std::nullopt;
        default:
            break;
    }

    const auto mantissa = (mantissaBits & 0x00800000u) != 0
                              ? static_cast<int32_t>(mantissaBits) - 0x01000000
                              : static_cast<int32_t>(mantissaBits);
    const auto exponent = static_cast<int8_t>(raw >> 24);

    // Divided rather than multiplied by a negative power: 0.1 is inexact, 10 is not.
    return exponent < 0 ? mantissa / std::pow(10.0, -exponent) : mantissa * std::pow(10.0, exponent);
}

}

std::optional<TemperatureMeasurement> decodeTemperatureMeasurement(const std::vector<uint8_t>& payload)
{
    ByteReader reader(payload);

    const auto flags = reader.u8();
    const auto value = ieee11073Float(reader.u32());

    if ((flags & FlagTimeStampPresent) != 0)
    {
        // Year (uint16), month, day, hours, minutes, seconds.
        reader.u16();
        for (int i = 0; i < 5; ++i)
            reader.u8();
    }

    TemperatureMeasurement measurement;

    if ((flags & FlagTemperatureTypePresent) != 0)
        measurement.temperatureType = reader.u8();

    if (reader.overran())
        return std::nullopt;

    if (value)
        measurement.celsius = (flags & FlagFahrenheit) != 0 ? (*value - 32.0) * 5.0 / 9.0 : *value;

    return measurement;
}

std::optional<TemperatureMeasurement> decodeEnvironmentalTemperature(const std::vector<uint8_t>& payload)
{
    if (payload.size() != 2)
        return std::nullopt;

    const auto raw = ByteReader(payload).s16();

    TemperatureMeasurement measurement;
    if (raw != EnvironmentalTemperatureUnknown)
        measurement.celsius = raw * 0.01;

    return measurement;
}

const char* temperatureTypeName(uint8_t value)
{
    switch (value)
    {
        case 1: return "Armpit";
        case 2: return "Body";
        case 3: return "Ear";
        case 4: return "Finger";
        case 5: return "Gastro-intestinal Tract";
        case 6: return "Mouth";
        case 7: return "Rectum";
        case 8: return "Toe";
        case 9: return "Tympanum";
        default: return "Unknown";
    }
}

}

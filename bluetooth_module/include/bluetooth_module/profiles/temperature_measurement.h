#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace opendaq_bt::profiles
{

/*!
 * One decoded temperature reading, always in degrees Celsius.
 *
 * @c celsius is empty when the peripheral sent a well-formed frame saying it has no value (an
 * IEEE-11073 NaN/NRes/INF, or the ESS "unknown" sentinel). That is not a malformed frame, so it
 * is kept apart from the decoders' nullopt.
 */
struct TemperatureMeasurement
{
    std::optional<double> celsius;

    /*! Temperature Type (0x2A1D) enum value, when the measurement carries one. */
    std::optional<uint8_t> temperatureType;
};

/*!
 * Decodes Temperature Measurement (0x2A1C) or Intermediate Temperature (0x2A1E), which share a
 * layout: flags, an IEEE-11073 32-bit FLOAT, an optional time stamp and an optional type.
 *
 * Fahrenheit is converted to Celsius. The time stamp is validated and skipped: it may be a stored
 * reading from hours ago, and the domain must stay monotonic.
 *
 * Returns nullopt for a payload whose flags promise more than the buffer holds.
 */
std::optional<TemperatureMeasurement> decodeTemperatureMeasurement(const std::vector<uint8_t>& payload);

/*!
 * Decodes Environmental Sensing Temperature (0x2A6E): a sint16 in 0.01 degC.
 *
 * Returns nullopt for any payload that is not exactly two octets.
 */
std::optional<TemperatureMeasurement> decodeEnvironmentalTemperature(const std::vector<uint8_t>& payload);

/*! Human-readable name for a Temperature Type (0x2A1D) enum value. */
const char* temperatureTypeName(uint8_t value);

}

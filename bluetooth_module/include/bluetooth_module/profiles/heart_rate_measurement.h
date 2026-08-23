#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace opendaq_bt::profiles
{

/*!
 * One decoded Heart Rate Measurement notification (Bluetooth SIG characteristic 0x2A37).
 *
 * Only the heart rate itself is always present. Sensor contact, energy expended and RR intervals
 * are optional and are signalled by flag bits in the first octet of the payload.
 */
struct HeartRateMeasurement
{
    uint16_t beatsPerMinute = 0;

    /*! Whether the peripheral implements contact detection at all, and if so what it currently sees. */
    bool sensorContactSupported = false;
    bool sensorContactDetected = false;

    /*! Cumulative energy expended in kilojoules, when the peripheral reports it. */
    std::optional<uint16_t> energyExpendedKj;

    /*!
     * Beat-to-beat intervals in seconds, oldest first. The wire format carries them in units of
     * 1/1024 s; they are converted here so consumers never see the raw ticks. A notification may
     * carry none, one, or several.
     */
    std::vector<double> rrIntervalsSeconds;
};

/*!
 * Decodes a Heart Rate Measurement payload.
 *
 * Returns nullopt for any payload that is empty, or whose flags promise more data than the buffer
 * actually holds. Real peripherals do emit short and malformed frames, and reading past the buffer
 * to satisfy a flag byte would be far worse than dropping the sample.
 */
std::optional<HeartRateMeasurement> decodeHeartRateMeasurement(const std::vector<uint8_t>& payload);

/*! Human-readable name for a Body Sensor Location (0x2A38) enum value. */
const char* bodySensorLocationName(uint8_t value);

}

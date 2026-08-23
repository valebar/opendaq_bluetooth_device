#include <bluetooth_module/profiles/heart_rate_measurement.h>

namespace opendaq_bt::profiles
{

namespace
{

// Flag bits in octet 0 of the Heart Rate Measurement characteristic (0x2A37).
constexpr uint8_t FlagValueFormatUint16 = 1u << 0;
constexpr uint8_t FlagSensorContactDetected = 1u << 1;
constexpr uint8_t FlagSensorContactSupported = 1u << 2;
constexpr uint8_t FlagEnergyExpendedPresent = 1u << 3;
constexpr uint8_t FlagRrIntervalPresent = 1u << 4;

/*! RR intervals and energy are transmitted little-endian, as is everything in GATT. */
uint16_t readUint16(const std::vector<uint8_t>& payload, size_t offset)
{
    return static_cast<uint16_t>(static_cast<uint16_t>(payload[offset]) |
                                 static_cast<uint16_t>(static_cast<uint16_t>(payload[offset + 1]) << 8));
}

/*! RR intervals are expressed in units of 1/1024 of a second. */
constexpr double RrIntervalTickSeconds = 1.0 / 1024.0;

}

std::optional<HeartRateMeasurement> decodeHeartRateMeasurement(const std::vector<uint8_t>& payload)
{
    if (payload.empty())
        return std::nullopt;

    const uint8_t flags = payload[0];
    size_t offset = 1;

    HeartRateMeasurement measurement;

    // Contact status is only meaningful when the peripheral says it supports detection; some
    // peripherals leave the "detected" bit set even with no electrode contact at all.
    measurement.sensorContactSupported = (flags & FlagSensorContactSupported) != 0;
    measurement.sensorContactDetected = measurement.sensorContactSupported &&
                                        (flags & FlagSensorContactDetected) != 0;

    if ((flags & FlagValueFormatUint16) != 0)
    {
        if (payload.size() < offset + 2)
            return std::nullopt;

        measurement.beatsPerMinute = readUint16(payload, offset);
        offset += 2;
    }
    else
    {
        if (payload.size() < offset + 1)
            return std::nullopt;

        measurement.beatsPerMinute = payload[offset];
        offset += 1;
    }

    if ((flags & FlagEnergyExpendedPresent) != 0)
    {
        if (payload.size() < offset + 2)
            return std::nullopt;

        measurement.energyExpendedKj = readUint16(payload, offset);
        offset += 2;
    }

    if ((flags & FlagRrIntervalPresent) != 0)
    {
        const size_t remaining = payload.size() - offset;

        // The flag promises at least one interval, and each is exactly two octets. A remainder
        // that is zero or odd means the frame is malformed, not that it holds a partial interval.
        if (remaining == 0 || remaining % 2 != 0)
            return std::nullopt;

        measurement.rrIntervalsSeconds.reserve(remaining / 2);
        for (; offset + 1 < payload.size(); offset += 2)
            measurement.rrIntervalsSeconds.push_back(readUint16(payload, offset) * RrIntervalTickSeconds);
    }

    return measurement;
}

const char* bodySensorLocationName(uint8_t value)
{
    switch (value)
    {
        case 0: return "Other";
        case 1: return "Chest";
        case 2: return "Wrist";
        case 3: return "Finger";
        case 4: return "Hand";
        case 5: return "Ear Lobe";
        case 6: return "Foot";
        default: return "Unknown";
    }
}

}

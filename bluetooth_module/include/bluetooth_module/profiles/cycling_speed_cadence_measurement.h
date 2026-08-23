#pragma once

#include <bluetooth_module/profiles/cycling_common.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opendaq_bt::profiles
{

/*!
 * One decoded CSC Measurement notification (Bluetooth SIG characteristic 0x2A5B).
 *
 * A single flags octet, then wheel data, then crank data. A sensor may carry either or both --
 * a cadence-only sensor sends crank data alone -- and a payload carrying neither is legal and
 * simply says nothing happened.
 *
 * Both event times tick at 1/1024 s here, including the wheel one. The identically named field in
 * Cycling Power ticks at 1/2048 s instead, which is the single easiest mistake to make across
 * these two services.
 */
struct CscMeasurement
{
    std::optional<RevolutionData> wheelRevolutions;
    std::optional<RevolutionData> crankRevolutions;
};

/*!
 * Decodes a CSC Measurement payload.
 *
 * Returns nullopt for an empty payload, or one whose flags promise data the buffer does not hold.
 */
std::optional<CscMeasurement> decodeCscMeasurement(const std::vector<uint8_t>& payload);

/*! Renders a CSC Feature (0x2A5C) bitmask as a readable list. */
std::string describeCscFeatures(uint16_t features);

}

#pragma once

#include <bluetooth_module/profiles/cycling_common.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opendaq_bt::profiles
{

/*!
 * One decoded Cycling Power Measurement notification (Bluetooth SIG characteristic 0x2A63).
 *
 * Only the instantaneous power is always present; everything else is announced by a flag bit in
 * the 16-bit flags field that opens the payload. Values are converted out of their wire units
 * here -- halves of a percent, thirty-seconds of a newton metre -- so that no consumer has to
 * know them.
 */
struct CyclingPowerMeasurement
{
    /*! Signed: a trainer coasting, or a pedal-based meter being back-pedalled, reports negative. */
    int16_t instantaneousPowerWatts = 0;

    /*! Share of the total produced by one leg. The reference below says which leg. */
    std::optional<double> pedalPowerBalancePercent;
    bool pedalPowerBalanceReferenceIsLeft = false;

    std::optional<double> accumulatedTorqueNm;
    bool accumulatedTorqueIsCrankBased = false;

    /*! Event time ticks are 1/2048 s for the wheel and 1/1024 s for the crank. Not a typo. */
    std::optional<RevolutionData> wheelRevolutions;
    std::optional<RevolutionData> crankRevolutions;

    std::optional<double> maximumForceNewtons;
    std::optional<double> minimumForceNewtons;

    std::optional<double> maximumTorqueNm;
    std::optional<double> minimumTorqueNm;

    /*! Crank angles at which the extreme magnitudes above occurred, in degrees. */
    std::optional<uint16_t> maximumAngleDegrees;
    std::optional<uint16_t> minimumAngleDegrees;

    std::optional<uint16_t> topDeadSpotAngleDegrees;
    std::optional<uint16_t> bottomDeadSpotAngleDegrees;

    std::optional<uint16_t> accumulatedEnergyKj;

    /*! The sensor is asking to be zero-offset calibrated. */
    bool offsetCompensationIndicator = false;
};

/*!
 * Decodes a Cycling Power Measurement payload.
 *
 * Returns nullopt for any payload whose flags promise more data than the buffer holds, which
 * includes one too short to carry the mandatory flags and power fields.
 */
std::optional<CyclingPowerMeasurement> decodeCyclingPowerMeasurement(const std::vector<uint8_t>& payload);

/*!
 * Renders a Cycling Power Feature (0x2A65) bitmask as a readable list.
 *
 * Bits 20-21 are a two-bit "Distributed System Support" field rather than two flags, so they are
 * deliberately left out rather than rendered as nonsense.
 */
std::string describeCyclingPowerFeatures(uint32_t features);

}

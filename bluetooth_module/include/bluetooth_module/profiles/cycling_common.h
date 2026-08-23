#pragma once

#include <cstdint>
#include <string>

namespace opendaq_bt::profiles
{

/*!
 * A cumulative revolution counter and the timestamp of the revolution that last incremented it.
 *
 * Both the Cycling Power and the Cycling Speed and Cadence service report wheel and crank motion
 * this way, and neither reports a rate. RevolutionTracker turns a pair of these into one.
 *
 * @c lastEventTime is a free-running 16-bit counter on the sensor. Its tick rate is NOT the same
 * everywhere: 1/1024 s for everything except the Cycling Power *wheel* event time, which is
 * 1/2048 s. The struct deliberately carries raw ticks rather than seconds so that the tick rate
 * stays the caller's business and cannot be silently assumed here.
 */
struct RevolutionData
{
    uint32_t cumulativeRevolutions = 0;
    uint16_t lastEventTime = 0;
};

/*!
 * Human-readable name for a Sensor Location (0x2A5D) enum value.
 *
 * Shared by the Cycling Power and Cycling Speed and Cadence services. This is a *different* enum
 * from Body Sensor Location (0x2A38), which the Heart Rate profile names -- the two overlap in
 * numbering and disagree on meaning, so they must never be merged.
 */
const char* cyclingSensorLocationName(uint8_t value);

/*! Renders a bitmask as ", "-separated names, or "None" when nothing is set. */
std::string describeBits(uint32_t bits, const char* const* names, size_t nameCount);

}

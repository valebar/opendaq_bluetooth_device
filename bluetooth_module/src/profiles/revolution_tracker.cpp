#include <bluetooth_module/profiles/revolution_tracker.h>

#include <algorithm>

namespace opendaq_bt::profiles
{

namespace
{

/*! The event time is a uint16 on the wire, whatever its tick rate, so it always wraps here. */
constexpr double EventTimeModulus = 1 << 16;

constexpr int64_t MicrosPerSecond = 1000000;

/*! Margin taken off the wrap period, because host arrival time over-reads the true event gap. */
constexpr int64_t ReAnchorMarginMicros = 2 * MicrosPerSecond;

/*!
 * Slack on the plausibility ceiling, in revolutions.
 *
 * One legitimate revolution can land inside an arbitrarily short interval -- the sensor decides
 * when to notify, not the rider -- so a ceiling expressed purely as a rate would reject it.
 */
constexpr double PlausibilitySlackRevolutions = 1.0;

}

RevolutionTracker::RevolutionTracker(Config config)
    : config(config)
    , reAnchorAfterMicros(std::max<int64_t>(
          MicrosPerSecond,
          static_cast<int64_t>(EventTimeModulus / config.eventTimeTicksPerSecond * MicrosPerSecond) -
              ReAnchorMarginMicros))
{
}

void RevolutionTracker::reset()
{
    anchored = false;
    movingSinceLastZero = false;
}

void RevolutionTracker::anchorAt(uint64_t cumulativeRevolutions,
                                 uint16_t lastEventTime,
                                 int64_t arrivalMicros)
{
    anchored = true;
    anchorRevolutions = cumulativeRevolutions % config.revolutionModulus;
    anchorEventTime = lastEventTime;
    anchorArrivalMicros = arrivalMicros;
}

std::optional<RevolutionTracker::Update> RevolutionTracker::update(uint64_t cumulativeRevolutions,
                                                                   uint16_t lastEventTime,
                                                                   int64_t arrivalMicros)
{
    if (!anchored)
    {
        anchorAt(cumulativeRevolutions, lastEventTime, arrivalMicros);
        return std::nullopt;
    }

    const auto sinceAnchor = arrivalMicros - anchorArrivalMicros;

    // Past one wrap of the event time the difference below is ambiguous: it could be that
    // interval, or that interval plus any number of whole wraps.
    if (sinceAnchor > reAnchorAfterMicros)
    {
        anchorAt(cumulativeRevolutions, lastEventTime, arrivalMicros);
        movingSinceLastZero = false;
        return std::nullopt;
    }

    // A stationary sensor repeats the last event time verbatim. There is no new revolution to
    // measure, and the anchor deliberately stays put: it marks the last time the sensor actually
    // turned, which is what both the wrap test above and the coast-down below measure from.
    const auto deltaTicks = static_cast<uint16_t>(lastEventTime - anchorEventTime);
    if (deltaTicks == 0)
    {
        const auto stoppedFor = static_cast<double>(sinceAnchor) / MicrosPerSecond;

        if (config.coastDownSeconds > 0.0 && movingSinceLastZero &&
            stoppedFor >= config.coastDownSeconds)
        {
            movingSinceLastZero = false;

            Update stopped;
            stopped.elapsedSeconds = stoppedFor;
            stopped.coastDown = true;
            return stopped;
        }

        return std::nullopt;
    }

    const auto current = cumulativeRevolutions % config.revolutionModulus;
    const auto deltaRevolutions =
        (current + config.revolutionModulus - anchorRevolutions) % config.revolutionModulus;

    Update result;
    result.elapsedSeconds = deltaTicks / config.eventTimeTicksPerSecond;
    result.revolutions = static_cast<double>(deltaRevolutions);
    result.revolutionsPerMinute = result.revolutions / result.elapsedSeconds * 60.0;

    // A counter that reset -- a sensor that changed its battery, or browned out and reconnected
    // inside the retry period -- looks like almost a whole modulus of revolutions in one interval.
    // The wrap rule above cannot see that, because barely any time passed.
    const auto plausibleRevolutions =
        config.maxRevolutionsPerSecond * result.elapsedSeconds + PlausibilitySlackRevolutions;
    if (result.revolutions > plausibleRevolutions)
    {
        anchorAt(cumulativeRevolutions, lastEventTime, arrivalMicros);
        movingSinceLastZero = false;
        return std::nullopt;
    }

    anchorAt(cumulativeRevolutions, lastEventTime, arrivalMicros);
    movingSinceLastZero = result.revolutions > 0.0;

    return result;
}

}

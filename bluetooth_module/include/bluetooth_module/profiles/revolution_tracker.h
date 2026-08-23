#pragma once

#include <cstdint>
#include <optional>

namespace opendaq_bt::profiles
{

/*!
 * Turns a cumulative revolution counter into a rate.
 *
 * Neither the Cycling Power nor the Cycling Speed and Cadence service transmits cadence or speed.
 * Both transmit a counter of revolutions since the sensor powered up plus the timestamp of the
 * most recent revolution, and the host differentiates. Four instances of that appear in this
 * module -- CSC wheel, CSC crank, CPS wheel, CPS crank -- differing only in how wide the counter
 * is and how fast the event clock ticks, so the arithmetic lives here once.
 *
 * Pure: no openDAQ, no BLE types, and no clock of its own. The host arrival time is an argument,
 * which keeps the whole wrap-and-reset matrix testable without a single sleep, and guarantees the
 * gap arithmetic here cannot disagree with the timestamps the channel stamps on its packets --
 * they are literally the same number.
 */
class RevolutionTracker
{
public:
    struct Config
    {
        /*! 1<<16 for a crank counter (uint16 on the wire), 1<<32 for a wheel counter. */
        uint64_t revolutionModulus = uint64_t{1} << 16;

        /*!
         * 1024 everywhere except the Cycling Power *wheel* event time, which is 2048. Getting
         * this wrong halves or doubles every rate with no other symptom, which is why it has no
         * useful default and every construction site spells it out.
         */
        double eventTimeTicksPerSecond = 1024.0;

        /*!
         * Rates above this are discarded and the tracker re-anchors instead.
         *
         * This is not belt and braces, it is the guard that catches the case the wrap rule below
         * cannot: a sensor that browns out and reconnects within seconds comes back with its
         * counter at zero, and modular arithmetic reads that as almost a full modulus of
         * revolutions in a fraction of a second. Around 5 rev/s for a crank (300 rpm, above any
         * human) and 70 rev/s for a wheel (over 500 km/h on a road tyre).
         */
        double maxRevolutionsPerSecond = 5.0;

        /*!
         * How long a stalled counter waits before one explicit zero is emitted. 0 disables it.
         *
         * A stationary sensor keeps notifying with an unchanged event time, so differentiation
         * alone makes the rate signal simply stop -- which a consumer reads as "the sensor died",
         * not "the rider stopped". One zero says the latter. It is no more synthetic than any
         * other sample here: the sensor never transmits a rate at all, and after three seconds
         * without a revolution the rate genuinely is zero.
         */
        double coastDownSeconds = 3.0;
    };

    explicit RevolutionTracker(Config config);

    struct Update
    {
        double revolutionsPerMinute = 0.0;

        /*! Revolutions in this interval, already corrected for counter wrap. */
        double revolutions = 0.0;

        double elapsedSeconds = 0.0;

        /*! A synthesised zero for a stopped sensor rather than a measured interval. */
        bool coastDown = false;
    };

    /*!
     * Feeds one measurement.
     *
     * @param arrivalMicros host arrival time of the notification. Used only to bound how long the
     *        16-bit event time has had to wrap, and to time the coast-down; the rate itself always
     *        comes from the sensor's own clock, which is far better than host arrival timing.
     *
     * Returns nullopt when there is nothing to report rather than inventing a sample: on the first
     * measurement, on a repeated event time before the coast-down elapses, and whenever the
     * tracker re-anchors.
     */
    std::optional<Update> update(uint64_t cumulativeRevolutions,
                                 uint16_t lastEventTime,
                                 int64_t arrivalMicros);

    /*! Forgets the anchor, so the next measurement starts a fresh interval. */
    void reset();

private:
    void anchorAt(uint64_t cumulativeRevolutions, uint16_t lastEventTime, int64_t arrivalMicros);

    const Config config;

    /*!
     * How long the host may go without a fresh event before the interval is abandoned.
     *
     * The event time is a uint16 whatever its rate, so it wraps after 65536 ticks: 64 s at 1024 Hz
     * and 32 s at 2048 Hz. The threshold sits a little short of that because BLE arrival time
     * systematically over-reads the true event gap, and the two errors are not symmetric --
     * re-anchoring one notification early costs a sample nobody misses, while re-anchoring one
     * notification late fabricates a spike that lands in every attached reader.
     */
    const int64_t reAnchorAfterMicros;

    bool anchored = false;
    uint64_t anchorRevolutions = 0;
    uint16_t anchorEventTime = 0;
    int64_t anchorArrivalMicros = 0;

    /*! Whether a zero is still owed for the current stop. Stops the coast-down repeating. */
    bool movingSinceLastZero = false;
};

}

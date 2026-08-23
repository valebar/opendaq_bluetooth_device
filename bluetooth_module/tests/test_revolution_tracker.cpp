#include <bluetooth_module/profiles/revolution_tracker.h>

#include <gtest/gtest.h>

using namespace opendaq_bt::profiles;

namespace
{

/*! Arrival timestamps are microseconds, the same unit the channels stamp on their packets. */
constexpr int64_t Second = 1000000;

/*! A crank counter: uint16 wide, 1/1024 s ticks, and nothing above 300 rpm is a human. */
RevolutionTracker::Config crankConfig()
{
    return {uint64_t{1} << 16, 1024.0, 5.0, 3.0};
}

/*! A wheel counter. The tick rate is the argument because CSC uses 1024 and CPS uses 2048. */
RevolutionTracker::Config wheelConfig(double ticksPerSecond)
{
    return {uint64_t{1} << 32, ticksPerSecond, 70.0, 3.0};
}

}

TEST(RevolutionTracker, FirstUpdateAnchorsAndEmitsNothing)
{
    RevolutionTracker tracker(crankConfig());

    // There is nothing to differentiate against yet, and a rate invented from one measurement
    // would be worse than the gap.
    EXPECT_FALSE(tracker.update(100, 0, 0));
}

TEST(RevolutionTracker, ComputesCadenceFromTheCrankCounter)
{
    RevolutionTracker tracker(crankConfig());

    tracker.update(100, 0, 0);

    // One revolution in 1024 ticks -- exactly one second -- is 60 rpm.
    const auto update = tracker.update(101, 1024, Second);

    ASSERT_TRUE(update);
    EXPECT_DOUBLE_EQ(update->revolutionsPerMinute, 60.0);
    EXPECT_DOUBLE_EQ(update->revolutions, 1.0);
    EXPECT_DOUBLE_EQ(update->elapsedSeconds, 1.0);
    EXPECT_FALSE(update->coastDown);
}

TEST(RevolutionTracker, ComputesTheRateAtTwoThousandFortyEightHertz)
{
    // The Cycling Power wheel event time. Reading it as 1024 Hz would report double the speed and
    // nothing else would look wrong, which is why this rate is a construction-site argument.
    RevolutionTracker tracker(wheelConfig(2048.0));

    tracker.update(0, 0, 0);

    const auto update = tracker.update(1, 2048, Second);

    ASSERT_TRUE(update);
    EXPECT_DOUBLE_EQ(update->revolutionsPerMinute, 60.0);
    EXPECT_DOUBLE_EQ(update->elapsedSeconds, 1.0);
}

TEST(RevolutionTracker, ComputesTheRateAcrossAnEventTimeWrap)
{
    RevolutionTracker tracker(crankConfig());

    tracker.update(0, 65000, 0);

    // 65000 -> 464 wraps the 16-bit event time; the true interval is 1000 ticks.
    const auto update = tracker.update(1, 464, Second);

    ASSERT_TRUE(update);
    EXPECT_DOUBLE_EQ(update->elapsedSeconds, 1000.0 / 1024.0);
    EXPECT_DOUBLE_EQ(update->revolutions, 1.0);
}

TEST(RevolutionTracker, HandlesTheCrankCounterWrappingAtSixteenBits)
{
    RevolutionTracker tracker(crankConfig());

    tracker.update(65535, 0, 0);

    const auto update = tracker.update(3, 1024, Second);

    ASSERT_TRUE(update);
    EXPECT_DOUBLE_EQ(update->revolutions, 4.0);
}

TEST(RevolutionTracker, HandlesTheWheelCounterWrappingAtThirtyTwoBits)
{
    RevolutionTracker tracker(wheelConfig(1024.0));

    tracker.update(4294967295u, 0, 0);

    const auto update = tracker.update(2, 1024, Second);

    ASSERT_TRUE(update);
    EXPECT_DOUBLE_EQ(update->revolutions, 3.0);
}

TEST(RevolutionTracker, EmitsNothingWhenTheEventTimeDidNotAdvance)
{
    RevolutionTracker tracker(crankConfig());

    tracker.update(100, 1024, 0);

    // A stationary sensor repeats the last event time verbatim. Dividing by that interval is a
    // division by zero, and treating it as a fresh sample would flood the signal with duplicates.
    EXPECT_FALSE(tracker.update(100, 1024, Second));
}

TEST(RevolutionTracker, EmitsOneZeroOnceTheCoastDownElapses)
{
    RevolutionTracker tracker(crankConfig());

    tracker.update(100, 0, 0);
    ASSERT_TRUE(tracker.update(101, 1024, Second));

    // Two seconds stopped is not yet a stop; the sensor may simply be between revolutions.
    EXPECT_FALSE(tracker.update(101, 1024, 3 * Second));

    const auto stopped = tracker.update(101, 1024, 5 * Second);

    ASSERT_TRUE(stopped);
    EXPECT_DOUBLE_EQ(stopped->revolutionsPerMinute, 0.0);
    EXPECT_TRUE(stopped->coastDown);
}

TEST(RevolutionTracker, EmitsNoFurtherZerosWhileTheSensorStaysStopped)
{
    RevolutionTracker tracker(crankConfig());

    tracker.update(100, 0, 0);
    tracker.update(101, 1024, Second);
    ASSERT_TRUE(tracker.update(101, 1024, 5 * Second));

    // One zero says the rider stopped. A zero a second for the rest of the ride says nothing.
    EXPECT_FALSE(tracker.update(101, 1024, 6 * Second));
    EXPECT_FALSE(tracker.update(101, 1024, 7 * Second));
}

TEST(RevolutionTracker, ResumesMeasuringAfterACoastDown)
{
    RevolutionTracker tracker(crankConfig());

    tracker.update(100, 0, 0);
    tracker.update(101, 1024, Second);
    ASSERT_TRUE(tracker.update(101, 1024, 5 * Second));

    // Pedalling again four seconds later: the sensor's own clock ran throughout, so the event
    // time has advanced by the whole stop.
    const auto moving = tracker.update(102, 1024 + 4096, 5 * Second + Second / 2);

    ASSERT_TRUE(moving);
    EXPECT_FALSE(moving->coastDown);
    EXPECT_DOUBLE_EQ(moving->revolutions, 1.0);
    EXPECT_DOUBLE_EQ(moving->elapsedSeconds, 4.0);
}

TEST(RevolutionTracker, ReAnchorsWithoutEmittingOnAGapLongerThanTheEventTimeWrap)
{
    RevolutionTracker tracker(crankConfig());

    tracker.update(100, 0, 0);

    // At 1024 Hz the event time wraps after 64 s, so past that a difference of event times could
    // be that interval or that interval plus any number of whole wraps.
    EXPECT_FALSE(tracker.update(200, 1024, 63 * Second));

    // ...and the tracker really did re-anchor rather than merely skip a sample.
    const auto update = tracker.update(201, 2048, 64 * Second);
    ASSERT_TRUE(update);
    EXPECT_DOUBLE_EQ(update->revolutions, 1.0);
}

TEST(RevolutionTracker, ReAnchorsSoonerAtTwoThousandFortyEightHertz)
{
    // The same 16-bit counter wraps in half the time when it ticks twice as fast.
    RevolutionTracker tracker(wheelConfig(2048.0));

    tracker.update(0, 0, 0);

    EXPECT_FALSE(tracker.update(100, 2048, 31 * Second));
}

TEST(RevolutionTracker, ReAnchorsWithoutEmittingWhenTheCounterResets)
{
    RevolutionTracker tracker(crankConfig());

    tracker.update(1000, 5000, 0);

    // A sensor that browned out and reconnected inside the reconnect window comes back with its
    // counter at zero. Modular arithmetic reads that as 64536 revolutions in one second, and the
    // gap rule cannot see it because barely any time passed. This is the guard that catches it.
    EXPECT_FALSE(tracker.update(0, 6024, 2 * Second));
}

TEST(RevolutionTracker, ReAnchorsWithoutEmittingOnAnImplausibleCadence)
{
    RevolutionTracker tracker(crankConfig());

    tracker.update(0, 0, 0);

    // 200 crank revolutions in one second is 12000 rpm.
    EXPECT_FALSE(tracker.update(200, 1024, Second));
}

TEST(RevolutionTracker, AcceptsASingleRevolutionInAVeryShortInterval)
{
    RevolutionTracker tracker(crankConfig());

    tracker.update(0, 0, 0);

    // The sensor chooses when to notify, so one legitimate revolution can land inside an
    // arbitrarily short interval. A ceiling expressed purely as a rate would reject it.
    const auto update = tracker.update(1, 64, Second / 16);

    ASSERT_TRUE(update);
    EXPECT_DOUBLE_EQ(update->revolutions, 1.0);
}

TEST(RevolutionTracker, ResetDiscardsTheAnchor)
{
    RevolutionTracker tracker(crankConfig());

    tracker.update(100, 0, 0);
    tracker.reset();

    EXPECT_FALSE(tracker.update(101, 1024, Second));
}

TEST(RevolutionTracker, ReportsZeroRevolutionsWhenTheEventTimeMovedButTheCounterDidNot)
{
    RevolutionTracker tracker(crankConfig());

    tracker.update(100, 0, 0);

    // Some sensors advance the event time on their own schedule rather than only on a revolution.
    // Zero revolutions over a real interval is a genuine zero rate, not a malformed frame.
    const auto update = tracker.update(100, 1024, Second);

    ASSERT_TRUE(update);
    EXPECT_DOUBLE_EQ(update->revolutionsPerMinute, 0.0);
    EXPECT_FALSE(update->coastDown);
}

#include <bluetooth_module/profiles/heart_rate_measurement.h>

#include <gtest/gtest.h>

using namespace opendaq_bt::profiles;

namespace
{

/*! Flag bits of Heart Rate Measurement (0x2A37), spelled out so the tests read as the spec does. */
constexpr uint8_t Uint16Format = 0x01;
constexpr uint8_t ContactDetected = 0x02;
constexpr uint8_t ContactSupported = 0x04;
constexpr uint8_t EnergyPresent = 0x08;
constexpr uint8_t RrPresent = 0x10;

}

TEST(HeartRateMeasurement, DecodesEightBitHeartRate)
{
    const auto decoded = decodeHeartRateMeasurement({0x00, 0x48});

    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->beatsPerMinute, 72);
    EXPECT_FALSE(decoded->sensorContactSupported);
    EXPECT_FALSE(decoded->energyExpendedKj);
    EXPECT_TRUE(decoded->rrIntervalsSeconds.empty());
}

TEST(HeartRateMeasurement, DecodesSixteenBitHeartRateLittleEndian)
{
    // 0x0136 == 310 bpm; the point is the byte order, not the plausibility.
    const auto decoded = decodeHeartRateMeasurement({Uint16Format, 0x36, 0x01});

    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->beatsPerMinute, 310);
}

TEST(HeartRateMeasurement, ReportsSensorContactOnlyWhenSupported)
{
    const auto supportedAndDetected =
        decodeHeartRateMeasurement({ContactSupported | ContactDetected, 0x40});
    ASSERT_TRUE(supportedAndDetected);
    EXPECT_TRUE(supportedAndDetected->sensorContactSupported);
    EXPECT_TRUE(supportedAndDetected->sensorContactDetected);

    const auto supportedNotDetected = decodeHeartRateMeasurement({ContactSupported, 0x40});
    ASSERT_TRUE(supportedNotDetected);
    EXPECT_TRUE(supportedNotDetected->sensorContactSupported);
    EXPECT_FALSE(supportedNotDetected->sensorContactDetected);

    // Some peripherals leave the "detected" bit set while advertising no support for detection.
    // Reporting contact on that basis would be misleading, so it must be ignored.
    const auto detectedWithoutSupport = decodeHeartRateMeasurement({ContactDetected, 0x40});
    ASSERT_TRUE(detectedWithoutSupport);
    EXPECT_FALSE(detectedWithoutSupport->sensorContactSupported);
    EXPECT_FALSE(detectedWithoutSupport->sensorContactDetected);
}

TEST(HeartRateMeasurement, DecodesEnergyExpended)
{
    // 0x0190 == 400 kJ
    const auto decoded = decodeHeartRateMeasurement({EnergyPresent, 0x40, 0x90, 0x01});

    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->beatsPerMinute, 64);
    ASSERT_TRUE(decoded->energyExpendedKj);
    EXPECT_EQ(*decoded->energyExpendedKj, 400);
}

TEST(HeartRateMeasurement, DecodesASingleRrIntervalIntoSeconds)
{
    // 1024 ticks of 1/1024 s is exactly one second.
    const auto decoded = decodeHeartRateMeasurement({RrPresent, 0x3C, 0x00, 0x04});

    ASSERT_TRUE(decoded);
    ASSERT_EQ(decoded->rrIntervalsSeconds.size(), 1u);
    EXPECT_DOUBLE_EQ(decoded->rrIntervalsSeconds[0], 1.0);
}

TEST(HeartRateMeasurement, DecodesSeveralRrIntervals)
{
    // 1024, 512 and 2048 ticks -> 1 s, 0.5 s, 2 s.
    const auto decoded =
        decodeHeartRateMeasurement({RrPresent, 0x3C, 0x00, 0x04, 0x00, 0x02, 0x00, 0x08});

    ASSERT_TRUE(decoded);
    ASSERT_EQ(decoded->rrIntervalsSeconds.size(), 3u);
    EXPECT_DOUBLE_EQ(decoded->rrIntervalsSeconds[0], 1.0);
    EXPECT_DOUBLE_EQ(decoded->rrIntervalsSeconds[1], 0.5);
    EXPECT_DOUBLE_EQ(decoded->rrIntervalsSeconds[2], 2.0);
}

TEST(HeartRateMeasurement, DecodesEnergyAndRrIntervalsTogether)
{
    // Field order is fixed by the spec: flags, heart rate, energy, then the RR intervals. Getting
    // the order wrong still parses, so this pins it down.
    const auto decoded = decodeHeartRateMeasurement(
        {static_cast<uint8_t>(Uint16Format | EnergyPresent | RrPresent),
         0x50, 0x00,          // heart rate 80, 16-bit
         0x2C, 0x01,          // energy 300 kJ
         0x00, 0x04,          // RR 1.0 s
         0x00, 0x02});        // RR 0.5 s

    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->beatsPerMinute, 80);
    ASSERT_TRUE(decoded->energyExpendedKj);
    EXPECT_EQ(*decoded->energyExpendedKj, 300);
    ASSERT_EQ(decoded->rrIntervalsSeconds.size(), 2u);
    EXPECT_DOUBLE_EQ(decoded->rrIntervalsSeconds[0], 1.0);
    EXPECT_DOUBLE_EQ(decoded->rrIntervalsSeconds[1], 0.5);
}

// ---- malformed payloads: these must be rejected, never read past the buffer -------------------

TEST(HeartRateMeasurement, RejectsEmptyPayload)
{
    EXPECT_FALSE(decodeHeartRateMeasurement({}));
}

TEST(HeartRateMeasurement, RejectsFlagsWithoutAValue)
{
    EXPECT_FALSE(decodeHeartRateMeasurement({0x00}));
}

TEST(HeartRateMeasurement, RejectsTruncatedSixteenBitHeartRate)
{
    EXPECT_FALSE(decodeHeartRateMeasurement({Uint16Format, 0x36}));
}

TEST(HeartRateMeasurement, RejectsTruncatedEnergyExpended)
{
    EXPECT_FALSE(decodeHeartRateMeasurement({EnergyPresent, 0x40, 0x90}));
}

TEST(HeartRateMeasurement, RejectsRrFlagWithNoIntervals)
{
    EXPECT_FALSE(decodeHeartRateMeasurement({RrPresent, 0x40}));
}

TEST(HeartRateMeasurement, RejectsRrIntervalCutInHalf)
{
    EXPECT_FALSE(decodeHeartRateMeasurement({RrPresent, 0x40, 0x00, 0x04, 0x00}));
}

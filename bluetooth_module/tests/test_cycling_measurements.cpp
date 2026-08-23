#include <bluetooth_module/profiles/cycling_common.h>
#include <bluetooth_module/profiles/cycling_power_measurement.h>
#include <bluetooth_module/profiles/cycling_speed_cadence_measurement.h>
#include <bluetooth_module/profiles/indoor_bike_data.h>

#include <gtest/gtest.h>

using namespace opendaq_bt::profiles;

namespace
{

/*! Flag bits of Cycling Power Measurement (0x2A63), low octet, spelled out as the spec has them. */
constexpr uint8_t CpPedalPowerBalance = 0x01;
constexpr uint8_t CpBalanceReferenceLeft = 0x02;
constexpr uint8_t CpAccumulatedTorque = 0x04;
constexpr uint8_t CpTorqueCrankBased = 0x08;
constexpr uint8_t CpWheelRevolutionData = 0x10;
constexpr uint8_t CpCrankRevolutionData = 0x20;
constexpr uint8_t CpExtremeForce = 0x40;
constexpr uint8_t CpExtremeTorque = 0x80;

// ...and of its high octet.
constexpr uint8_t CpExtremeAngles = 0x01;
constexpr uint8_t CpTopDeadSpot = 0x02;
constexpr uint8_t CpBottomDeadSpot = 0x04;
constexpr uint8_t CpAccumulatedEnergy = 0x08;
constexpr uint8_t CpOffsetCompensation = 0x10;

/*! Flag bits of CSC Measurement (0x2A5B). */
constexpr uint8_t CscWheelData = 0x01;
constexpr uint8_t CscCrankData = 0x02;

/*! Flag bits of Indoor Bike Data (0x2AD2), low octet. */
constexpr uint8_t IbMoreData = 0x01;
constexpr uint8_t IbAverageSpeed = 0x02;
constexpr uint8_t IbInstantaneousCadence = 0x04;
constexpr uint8_t IbAverageCadence = 0x08;
constexpr uint8_t IbTotalDistance = 0x10;
constexpr uint8_t IbResistanceLevel = 0x20;
constexpr uint8_t IbInstantaneousPower = 0x40;
constexpr uint8_t IbAveragePower = 0x80;

// ...and of its high octet.
constexpr uint8_t IbExpendedEnergy = 0x01;
constexpr uint8_t IbHeartRate = 0x02;
constexpr uint8_t IbMetabolicEquivalent = 0x04;
constexpr uint8_t IbElapsedTime = 0x08;
constexpr uint8_t IbRemainingTime = 0x10;

}

// ============================================================================================
// Cycling Power Measurement -- 0x2A63
// ============================================================================================

TEST(CyclingPowerMeasurement, DecodesTheMandatoryFieldsAlone)
{
    const auto decoded = decodeCyclingPowerMeasurement({0x00, 0x00, 0xC8, 0x00});

    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->instantaneousPowerWatts, 200);
    EXPECT_FALSE(decoded->pedalPowerBalancePercent);
    EXPECT_FALSE(decoded->crankRevolutions);
}

TEST(CyclingPowerMeasurement, DecodesNegativePower)
{
    // Signed on purpose: a trainer being back-pedalled, or a coasting pedal meter, reports below
    // zero, and reading the field as unsigned would turn -1 W into 65535 W.
    const auto decoded = decodeCyclingPowerMeasurement({0x00, 0x00, 0xFF, 0xFF});

    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->instantaneousPowerWatts, -1);
}

TEST(CyclingPowerMeasurement, DecodesPedalPowerBalanceAtHalfPercentResolution)
{
    // 0x4B == 75 half-percents == 37.5 %.
    const auto decoded = decodeCyclingPowerMeasurement({CpPedalPowerBalance, 0x00, 0xC8, 0x00, 0x4B});

    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded->pedalPowerBalancePercent);
    EXPECT_DOUBLE_EQ(*decoded->pedalPowerBalancePercent, 37.5);
    EXPECT_FALSE(decoded->pedalPowerBalanceReferenceIsLeft);
}

TEST(CyclingPowerMeasurement, ReportsThePedalPowerBalanceReference)
{
    const auto decoded = decodeCyclingPowerMeasurement(
        {static_cast<uint8_t>(CpPedalPowerBalance | CpBalanceReferenceLeft), 0x00, 0xC8, 0x00, 0x4B});

    ASSERT_TRUE(decoded);
    EXPECT_TRUE(decoded->pedalPowerBalanceReferenceIsLeft);
}

TEST(CyclingPowerMeasurement, DecodesAccumulatedTorqueAtOneThirtySecondResolution)
{
    // 0x0040 == 64 thirty-seconds == 2 N.m.
    const auto decoded =
        decodeCyclingPowerMeasurement({CpAccumulatedTorque, 0x00, 0xC8, 0x00, 0x40, 0x00});

    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded->accumulatedTorqueNm);
    EXPECT_DOUBLE_EQ(*decoded->accumulatedTorqueNm, 2.0);
    EXPECT_FALSE(decoded->accumulatedTorqueIsCrankBased);
}

TEST(CyclingPowerMeasurement, ReportsTheAccumulatedTorqueSource)
{
    const auto decoded = decodeCyclingPowerMeasurement(
        {static_cast<uint8_t>(CpAccumulatedTorque | CpTorqueCrankBased), 0x00, 0xC8, 0x00, 0x40, 0x00});

    ASSERT_TRUE(decoded);
    EXPECT_TRUE(decoded->accumulatedTorqueIsCrankBased);
}

TEST(CyclingPowerMeasurement, DecodesWheelRevolutionData)
{
    const auto decoded = decodeCyclingPowerMeasurement({CpWheelRevolutionData, 0x00,
                                                        0xC8, 0x00,              // 200 W
                                                        0x10, 0x27, 0x00, 0x00,  // 10000 revolutions
                                                        0x00, 0x08});            // event time 2048

    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded->wheelRevolutions);
    EXPECT_EQ(decoded->wheelRevolutions->cumulativeRevolutions, 10000u);
    EXPECT_EQ(decoded->wheelRevolutions->lastEventTime, 2048u);
}

TEST(CyclingPowerMeasurement, DecodesCrankRevolutionData)
{
    const auto decoded = decodeCyclingPowerMeasurement({CpCrankRevolutionData, 0x00,
                                                        0xC8, 0x00,    // 200 W
                                                        0xE8, 0x03,    // 1000 revolutions
                                                        0x00, 0x04});  // event time 1024

    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded->crankRevolutions);
    EXPECT_EQ(decoded->crankRevolutions->cumulativeRevolutions, 1000u);
    EXPECT_EQ(decoded->crankRevolutions->lastEventTime, 1024u);
}

TEST(CyclingPowerMeasurement, DecodesBothRevolutionBlocksInWireOrder)
{
    // Wheel comes before crank. Swapping them decodes without error and reports the wheel counter
    // as a cadence, so only an explicit ordering test catches it.
    const auto decoded = decodeCyclingPowerMeasurement(
        {static_cast<uint8_t>(CpWheelRevolutionData | CpCrankRevolutionData), 0x00,
         0xC8, 0x00,
         0x10, 0x27, 0x00, 0x00, 0x00, 0x08,   // wheel: 10000 revolutions at tick 2048
         0xE8, 0x03, 0x00, 0x04});             // crank: 1000 revolutions at tick 1024

    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded->wheelRevolutions);
    ASSERT_TRUE(decoded->crankRevolutions);
    EXPECT_EQ(decoded->wheelRevolutions->cumulativeRevolutions, 10000u);
    EXPECT_EQ(decoded->crankRevolutions->cumulativeRevolutions, 1000u);
}

TEST(CyclingPowerMeasurement, DecodesSignedExtremeForceMagnitudes)
{
    const auto decoded = decodeCyclingPowerMeasurement({CpExtremeForce, 0x00,
                                                        0xC8, 0x00,
                                                        0x2C, 0x01,    // 300 N
                                                        0x9C, 0xFF});  // -100 N

    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded->maximumForceNewtons);
    EXPECT_DOUBLE_EQ(*decoded->maximumForceNewtons, 300.0);
    EXPECT_DOUBLE_EQ(*decoded->minimumForceNewtons, -100.0);
}

TEST(CyclingPowerMeasurement, DecodesSignedExtremeTorqueMagnitudes)
{
    const auto decoded = decodeCyclingPowerMeasurement({CpExtremeTorque, 0x00,
                                                        0xC8, 0x00,
                                                        0x40, 0x00,    // 64/32 == 2 N.m
                                                        0xE0, 0xFF});  // -32/32 == -1 N.m

    ASSERT_TRUE(decoded);
    EXPECT_DOUBLE_EQ(*decoded->maximumTorqueNm, 2.0);
    EXPECT_DOUBLE_EQ(*decoded->minimumTorqueNm, -1.0);
}

TEST(CyclingPowerMeasurement, UnpacksExtremeAnglesFromATwelveBitPair)
{
    // Three octets carrying two 12-bit angles: maximum in the low twelve bits, minimum above it.
    // 0xF01234 -> maximum 0x234 (564), minimum 0xF01 (3841).
    const auto decoded =
        decodeCyclingPowerMeasurement({0x00, CpExtremeAngles, 0xC8, 0x00, 0x34, 0x12, 0xF0});

    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded->maximumAngleDegrees);
    EXPECT_EQ(*decoded->maximumAngleDegrees, 0x234);
    EXPECT_EQ(*decoded->minimumAngleDegrees, 0xF01);
}

TEST(CyclingPowerMeasurement, DecodesDeadSpotAnglesAndEnergy)
{
    const auto decoded = decodeCyclingPowerMeasurement(
        {0x00, static_cast<uint8_t>(CpTopDeadSpot | CpBottomDeadSpot | CpAccumulatedEnergy),
         0xC8, 0x00,
         0x1E, 0x00,    // top dead spot 30 degrees
         0xC8, 0x00,    // bottom dead spot 200 degrees
         0x2C, 0x01});  // 300 kJ

    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded->topDeadSpotAngleDegrees, 30u);
    EXPECT_EQ(*decoded->bottomDeadSpotAngleDegrees, 200u);
    EXPECT_EQ(*decoded->accumulatedEnergyKj, 300u);
}

TEST(CyclingPowerMeasurement, ReportsTheOffsetCompensationIndicator)
{
    const auto decoded = decodeCyclingPowerMeasurement({0x00, CpOffsetCompensation, 0xC8, 0x00});

    ASSERT_TRUE(decoded);
    EXPECT_TRUE(decoded->offsetCompensationIndicator);
}

TEST(CyclingPowerMeasurement, DecodesEveryOptionalFieldInOneNotification)
{
    // The offset-ordering test. Every presence bit set, so a field decoded in the wrong order or
    // at the wrong width shifts everything behind it and at least one assertion below fails.
    const auto decoded = decodeCyclingPowerMeasurement({
        0xFF, 0x1F,                              // every flag bit 0..12
        0xC8, 0x00,                              // 200 W
        0x4B,                                    // balance 37.5 %
        0x40, 0x00,                              // torque 2 N.m
        0x10, 0x27, 0x00, 0x00, 0x00, 0x08,      // wheel 10000 @ 2048
        0xE8, 0x03, 0x00, 0x04,                  // crank 1000 @ 1024
        0x2C, 0x01, 0x9C, 0xFF,                  // force 300 / -100 N
        0x40, 0x00, 0xE0, 0xFF,                  // torque 2 / -1 N.m
        0x34, 0x12, 0xF0,                        // angles 0x234 / 0xF01
        0x1E, 0x00,                              // top dead spot 30
        0xC8, 0x00,                              // bottom dead spot 200
        0x2C, 0x01});                            // energy 300 kJ

    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->instantaneousPowerWatts, 200);
    EXPECT_DOUBLE_EQ(*decoded->pedalPowerBalancePercent, 37.5);
    EXPECT_DOUBLE_EQ(*decoded->accumulatedTorqueNm, 2.0);
    EXPECT_EQ(decoded->wheelRevolutions->cumulativeRevolutions, 10000u);
    EXPECT_EQ(decoded->wheelRevolutions->lastEventTime, 2048u);
    EXPECT_EQ(decoded->crankRevolutions->cumulativeRevolutions, 1000u);
    EXPECT_EQ(decoded->crankRevolutions->lastEventTime, 1024u);
    EXPECT_DOUBLE_EQ(*decoded->maximumForceNewtons, 300.0);
    EXPECT_DOUBLE_EQ(*decoded->minimumForceNewtons, -100.0);
    EXPECT_DOUBLE_EQ(*decoded->maximumTorqueNm, 2.0);
    EXPECT_DOUBLE_EQ(*decoded->minimumTorqueNm, -1.0);
    EXPECT_EQ(*decoded->maximumAngleDegrees, 0x234);
    EXPECT_EQ(*decoded->minimumAngleDegrees, 0xF01);
    EXPECT_EQ(*decoded->topDeadSpotAngleDegrees, 30u);
    EXPECT_EQ(*decoded->bottomDeadSpotAngleDegrees, 200u);
    EXPECT_EQ(*decoded->accumulatedEnergyKj, 300u);
    EXPECT_TRUE(decoded->offsetCompensationIndicator);
}

TEST(CyclingPowerMeasurement, DescribesTheFeatureBits)
{
    // Bit 0 and bit 19; bits 20-21 are a two-bit field and are deliberately not rendered.
    EXPECT_EQ(describeCyclingPowerFeatures(0x1u), "Pedal Power Balance");
    EXPECT_EQ(describeCyclingPowerFeatures(0u), "None");
    EXPECT_EQ(describeCyclingPowerFeatures(1u << 19), "Enhanced Offset Compensation");
    EXPECT_EQ(describeCyclingPowerFeatures(3u << 20), "None");
}

// ---- malformed payloads: these must be rejected, never read past the buffer -----------------

TEST(CyclingPowerMeasurement, RejectsPayloadsShorterThanTheMandatoryFields)
{
    EXPECT_FALSE(decodeCyclingPowerMeasurement({}));
    EXPECT_FALSE(decodeCyclingPowerMeasurement({0x00}));
    EXPECT_FALSE(decodeCyclingPowerMeasurement({0x00, 0x00}));
    EXPECT_FALSE(decodeCyclingPowerMeasurement({0x00, 0x00, 0xC8}));
}

TEST(CyclingPowerMeasurement, RejectsAFlagPromisingMoreThanTheBufferHolds)
{
    EXPECT_FALSE(decodeCyclingPowerMeasurement({CpPedalPowerBalance, 0x00, 0xC8, 0x00}));
    EXPECT_FALSE(decodeCyclingPowerMeasurement({CpAccumulatedTorque, 0x00, 0xC8, 0x00, 0x40}));
    EXPECT_FALSE(decodeCyclingPowerMeasurement(
        {CpWheelRevolutionData, 0x00, 0xC8, 0x00, 0x10, 0x27, 0x00, 0x00, 0x00}));
    EXPECT_FALSE(
        decodeCyclingPowerMeasurement({CpCrankRevolutionData, 0x00, 0xC8, 0x00, 0xE8, 0x03, 0x00}));
    EXPECT_FALSE(decodeCyclingPowerMeasurement({CpExtremeForce, 0x00, 0xC8, 0x00, 0x2C, 0x01, 0x9C}));

    // The packed angle block is three octets, not two -- the easiest width to get wrong.
    EXPECT_FALSE(decodeCyclingPowerMeasurement({0x00, CpExtremeAngles, 0xC8, 0x00, 0x34, 0x12}));
    EXPECT_FALSE(decodeCyclingPowerMeasurement({0x00, CpAccumulatedEnergy, 0xC8, 0x00, 0x2C}));
}

TEST(CyclingPowerMeasurement, AcceptsTrailingOctetsItDoesNotUnderstand)
{
    // A longer frame than the flags call for is tolerated, not rejected: a future field this
    // module does not know about must not cost the fields it does.
    const auto decoded = decodeCyclingPowerMeasurement({0x00, 0x00, 0xC8, 0x00, 0xDE, 0xAD});

    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->instantaneousPowerWatts, 200);
}

// ============================================================================================
// CSC Measurement -- 0x2A5B
// ============================================================================================

TEST(CscMeasurement, DecodesWheelDataAlone)
{
    const auto decoded = decodeCscMeasurement({CscWheelData, 0x10, 0x27, 0x00, 0x00, 0x00, 0x04});

    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded->wheelRevolutions);
    EXPECT_FALSE(decoded->crankRevolutions);
    EXPECT_EQ(decoded->wheelRevolutions->cumulativeRevolutions, 10000u);
    EXPECT_EQ(decoded->wheelRevolutions->lastEventTime, 1024u);
}

TEST(CscMeasurement, DecodesCrankDataAlone)
{
    const auto decoded = decodeCscMeasurement({CscCrankData, 0xE8, 0x03, 0x00, 0x04});

    ASSERT_TRUE(decoded);
    EXPECT_FALSE(decoded->wheelRevolutions);
    ASSERT_TRUE(decoded->crankRevolutions);
    EXPECT_EQ(decoded->crankRevolutions->cumulativeRevolutions, 1000u);
}

TEST(CscMeasurement, DecodesBothBlocksInWireOrder)
{
    const auto decoded =
        decodeCscMeasurement({static_cast<uint8_t>(CscWheelData | CscCrankData),
                              0x10, 0x27, 0x00, 0x00, 0x00, 0x04,   // wheel 10000 @ 1024
                              0xE8, 0x03, 0x00, 0x08});             // crank 1000 @ 2048

    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->wheelRevolutions->cumulativeRevolutions, 10000u);
    EXPECT_EQ(decoded->wheelRevolutions->lastEventTime, 1024u);
    EXPECT_EQ(decoded->crankRevolutions->cumulativeRevolutions, 1000u);
    EXPECT_EQ(decoded->crankRevolutions->lastEventTime, 2048u);
}

TEST(CscMeasurement, AcceptsAFrameCarryingNeitherBlock)
{
    // Legal, and it simply says nothing has happened.
    const auto decoded = decodeCscMeasurement({0x00});

    ASSERT_TRUE(decoded);
    EXPECT_FALSE(decoded->wheelRevolutions);
    EXPECT_FALSE(decoded->crankRevolutions);
}

TEST(CscMeasurement, RejectsTruncatedFrames)
{
    EXPECT_FALSE(decodeCscMeasurement({}));
    EXPECT_FALSE(decodeCscMeasurement({CscWheelData, 0x10, 0x27, 0x00, 0x00, 0x00}));
    EXPECT_FALSE(decodeCscMeasurement({CscCrankData, 0xE8, 0x03, 0x00}));
    EXPECT_FALSE(decodeCscMeasurement({static_cast<uint8_t>(CscWheelData | CscCrankData),
                                       0x10, 0x27, 0x00, 0x00, 0x00, 0x04, 0xE8, 0x03, 0x00}));
}

TEST(CscMeasurement, DescribesTheFeatureBits)
{
    EXPECT_EQ(describeCscFeatures(0x03), "Wheel Revolution Data, Crank Revolution Data");
    EXPECT_EQ(describeCscFeatures(0x00), "None");
}

TEST(CyclingSensorLocation, NamesEveryDefinedLocation)
{
    // 0x2A5D, and NOT 0x2A38 Body Sensor Location, which numbers a different set of places.
    EXPECT_STREQ(cyclingSensorLocationName(0), "Other");
    EXPECT_STREQ(cyclingSensorLocationName(5), "Left Crank");
    EXPECT_STREQ(cyclingSensorLocationName(12), "Rear Wheel");
    EXPECT_STREQ(cyclingSensorLocationName(16), "Chain Ring");
}

TEST(CyclingSensorLocation, ReportsUnknownForReservedValues)
{
    EXPECT_STREQ(cyclingSensorLocationName(17), "Unknown");
    EXPECT_STREQ(cyclingSensorLocationName(255), "Unknown");
}

// ============================================================================================
// Indoor Bike Data -- 0x2AD2
// ============================================================================================

TEST(IndoorBikeData, DecodesInstantaneousSpeedWhenMoreDataIsClear)
{
    // Bit 0 is inverted: the speed is present when "More Data" is CLEAR. Reading it the intuitive
    // way shifts every field behind it by two octets and yields plausible nonsense.
    const auto decoded = decodeIndoorBikeData({0x00, 0x00, 0xB8, 0x0B});

    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded->instantaneousSpeedKph);
    EXPECT_DOUBLE_EQ(*decoded->instantaneousSpeedKph, 30.0);
}

TEST(IndoorBikeData, OmitsInstantaneousSpeedWhenMoreDataIsSet)
{
    const auto decoded = decodeIndoorBikeData({IbMoreData, 0x00});

    ASSERT_TRUE(decoded);
    EXPECT_FALSE(decoded->instantaneousSpeedKph);
}

TEST(IndoorBikeData, DecodesCadenceAtHalfRpmResolution)
{
    // 180 half-rpm == 90 rpm. Bit 2 has ordinary polarity, unlike bit 0.
    const auto decoded = decodeIndoorBikeData(
        {static_cast<uint8_t>(IbMoreData | IbInstantaneousCadence), 0x00, 0xB4, 0x00});

    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded->instantaneousCadenceRpm);
    EXPECT_DOUBLE_EQ(*decoded->instantaneousCadenceRpm, 90.0);
}

TEST(IndoorBikeData, DecodesTotalDistanceAsAThreeOctetField)
{
    const auto decoded =
        decodeIndoorBikeData({static_cast<uint8_t>(IbMoreData | IbTotalDistance), 0x00,
                              0xFF, 0xFF, 0xFF});

    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded->totalDistanceMetres, 16777215u);
}

TEST(IndoorBikeData, DecodesNegativeResistanceAndPower)
{
    const auto decoded = decodeIndoorBikeData(
        {static_cast<uint8_t>(IbMoreData | IbResistanceLevel | IbInstantaneousPower), 0x00,
         0xFB, 0xFF,     // resistance level -5
         0xCE, 0xFF});   // -50 W

    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded->resistanceLevel, -5);
    EXPECT_EQ(*decoded->instantaneousPowerWatts, -50);
}

TEST(IndoorBikeData, DecodesTheExpendedEnergyTriple)
{
    const auto decoded = decodeIndoorBikeData({IbMoreData, IbExpendedEnergy,
                                               0x2C, 0x01,   // 300 kcal total
                                               0xF4, 0x01,   // 500 kcal/h
                                               0x08});       // 8 kcal/min

    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded->totalEnergyKcal, 300u);
    EXPECT_EQ(*decoded->energyPerHourKcal, 500u);
    EXPECT_EQ(*decoded->energyPerMinuteKcal, 8u);
}

TEST(IndoorBikeData, TreatsEachEnergySentinelAsAbsentIndependently)
{
    // A machine that keeps a running total but cannot compute a per-minute rate sends the
    // all-ones pattern for that one field, every second, forever. Emitting it would be 255 kcal
    // a minute; the other two fields in the same block are still real.
    const auto decoded = decodeIndoorBikeData({IbMoreData, IbExpendedEnergy,
                                               0x2C, 0x01,
                                               0xFF, 0xFF,   // per hour: not available
                                               0xFF});       // per minute: not available

    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded->totalEnergyKcal, 300u);
    EXPECT_FALSE(decoded->energyPerHourKcal);
    EXPECT_FALSE(decoded->energyPerMinuteKcal);
}

TEST(IndoorBikeData, DecodesMetabolicEquivalentAtOneTenthResolution)
{
    const auto decoded =
        decodeIndoorBikeData({IbMoreData, IbMetabolicEquivalent, 0x64});

    ASSERT_TRUE(decoded);
    EXPECT_DOUBLE_EQ(*decoded->metabolicEquivalent, 10.0);
}

TEST(IndoorBikeData, DecodesEveryFieldInOneNotification)
{
    // Every flag except More Data, which must stay clear for the speed to be present. Any field
    // decoded at the wrong width shifts the rest and breaks at least one assertion below.
    const auto decoded = decodeIndoorBikeData({
        0xFE, 0x1F,          // bits 1..12 set, bit 0 clear
        0xB8, 0x0B,          // instantaneous speed 30.00 km/h
        0x10, 0x0E,          // average speed 36.00 km/h
        0xB4, 0x00,          // instantaneous cadence 90 rpm
        0xA0, 0x00,          // average cadence 80 rpm
        0xE8, 0x03, 0x00,    // total distance 1000 m
        0x07, 0x00,          // resistance level 7
        0xC8, 0x00,          // instantaneous power 200 W
        0xAF, 0x00,          // average power 175 W
        0x2C, 0x01,          // total energy 300 kcal
        0xF4, 0x01,          // energy per hour 500 kcal
        0x08,                // energy per minute 8 kcal
        0x8C,                // heart rate 140 bpm
        0x64,                // metabolic equivalent 10.0
        0x84, 0x03,          // elapsed 900 s
        0xB4, 0x00});        // remaining 180 s

    ASSERT_TRUE(decoded);
    EXPECT_DOUBLE_EQ(*decoded->instantaneousSpeedKph, 30.0);
    EXPECT_DOUBLE_EQ(*decoded->averageSpeedKph, 36.0);
    EXPECT_DOUBLE_EQ(*decoded->instantaneousCadenceRpm, 90.0);
    EXPECT_DOUBLE_EQ(*decoded->averageCadenceRpm, 80.0);
    EXPECT_EQ(*decoded->totalDistanceMetres, 1000u);
    EXPECT_EQ(*decoded->resistanceLevel, 7);
    EXPECT_EQ(*decoded->instantaneousPowerWatts, 200);
    EXPECT_EQ(*decoded->averagePowerWatts, 175);
    EXPECT_EQ(*decoded->totalEnergyKcal, 300u);
    EXPECT_EQ(*decoded->energyPerHourKcal, 500u);
    EXPECT_EQ(*decoded->energyPerMinuteKcal, 8u);
    EXPECT_EQ(*decoded->heartRateBpm, 140u);
    EXPECT_DOUBLE_EQ(*decoded->metabolicEquivalent, 10.0);
    EXPECT_EQ(*decoded->elapsedTimeSeconds, 900u);
    EXPECT_EQ(*decoded->remainingTimeSeconds, 180u);
}

TEST(IndoorBikeData, RejectsTruncatedFrames)
{
    EXPECT_FALSE(decodeIndoorBikeData({}));
    EXPECT_FALSE(decodeIndoorBikeData({0x00}));
    EXPECT_FALSE(decodeIndoorBikeData({0x00, 0x00, 0xB8}));

    // uint24 truncated to two octets, and the five-octet energy block truncated to four.
    EXPECT_FALSE(decodeIndoorBikeData(
        {static_cast<uint8_t>(IbMoreData | IbTotalDistance), 0x00, 0xE8, 0x03}));
    EXPECT_FALSE(decodeIndoorBikeData({IbMoreData, IbExpendedEnergy, 0x2C, 0x01, 0xF4, 0x01}));
}

// ============================================================================================
// Fitness Machine feature, ranges, status and control point -- 0x2ACC / 0x2AD6 / 0x2AD8 / 0x2AD9
// ============================================================================================

TEST(FitnessMachineFeature, DecodesTwoWords)
{
    const auto decoded = decodeFitnessMachineFeature(
        {0x02, 0x40, 0x00, 0x00,    // machine: Cadence + Power Measurement
         0x0C, 0x20, 0x00, 0x00});  // targets: Resistance + Power + Indoor Bike Simulation

    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->machineFeatures, 0x00004002u);
    EXPECT_EQ(decoded->targetSettingFeatures, 0x0000200Cu);
    EXPECT_TRUE((decoded->targetSettingFeatures & TargetSettingPower) != 0);
    EXPECT_TRUE((decoded->targetSettingFeatures & TargetSettingResistance) != 0);
    EXPECT_TRUE((decoded->targetSettingFeatures & TargetSettingIndoorBikeSimulation) != 0);
}

TEST(FitnessMachineFeature, RejectsAShortRead)
{
    EXPECT_FALSE(decodeFitnessMachineFeature({0x02, 0x40, 0x00, 0x00, 0x0C, 0x20, 0x00}));
}

TEST(FitnessMachineFeature, DescribesBothWords)
{
    EXPECT_EQ(describeFitnessMachineFeatures(1u << 14), "Power Measurement");
    EXPECT_EQ(describeTargetSettingFeatures(TargetSettingPower), "Power Target");
    EXPECT_EQ(describeTargetSettingFeatures(TargetSettingIndoorBikeSimulation),
              "Indoor Bike Simulation");
}

TEST(FitnessMachineRanges, DecodesTheSupportedPowerRangeWithASignedMinimum)
{
    const auto decoded = decodeSupportedPowerRange({0xCE, 0xFF,    // -50 W
                                                    0xE8, 0x03,    // 1000 W
                                                    0x01, 0x00});  // 1 W increment

    ASSERT_TRUE(decoded);
    EXPECT_DOUBLE_EQ(decoded->minimum, -50.0);
    EXPECT_DOUBLE_EQ(decoded->maximum, 1000.0);
    EXPECT_DOUBLE_EQ(decoded->increment, 1.0);
}

TEST(FitnessMachineRanges, DecodesTheResistanceRangeAtOneTenthResolution)
{
    const auto decoded = decodeSupportedResistanceLevelRange({0x00, 0x00,    // 0.0
                                                              0xFF, 0x00,    // 25.5
                                                              0x01, 0x00});  // 0.1

    ASSERT_TRUE(decoded);
    EXPECT_DOUBLE_EQ(decoded->minimum, 0.0);
    EXPECT_DOUBLE_EQ(decoded->maximum, 25.5);
    EXPECT_DOUBLE_EQ(decoded->increment, 0.1);
}

TEST(FitnessMachineRanges, RejectAShortRead)
{
    EXPECT_FALSE(decodeSupportedPowerRange({0xCE, 0xFF, 0xE8, 0x03, 0x01}));
    EXPECT_FALSE(decodeSupportedResistanceLevelRange({}));
}

TEST(TrainingStatus, DecodesTheStatusPastItsFlagsOctet)
{
    const auto status = decodeTrainingStatus({0x00, 0x0C});

    ASSERT_TRUE(status);
    EXPECT_EQ(*status, 0x0Cu);
    EXPECT_STREQ(trainingStatusName(*status), "Watt Control");
}

TEST(TrainingStatus, IgnoresATrailingStatusString)
{
    const auto status = decodeTrainingStatus({0x01, 0x02, 'W', 'a', 'r', 'm'});

    ASSERT_TRUE(status);
    EXPECT_STREQ(trainingStatusName(*status), "Warming Up");
}

TEST(TrainingStatus, RejectsAPayloadWithoutAStatusOctet)
{
    EXPECT_FALSE(decodeTrainingStatus({}));
    EXPECT_FALSE(decodeTrainingStatus({0x00}));
}

TEST(FitnessMachineControlPoint, EncodesTheParameterlessRequests)
{
    EXPECT_EQ(encodeRequestControl(), (std::vector<uint8_t>{0x00}));
    EXPECT_EQ(encodeReset(), (std::vector<uint8_t>{0x01}));
    EXPECT_EQ(encodeStartOrResume(), (std::vector<uint8_t>{0x07}));
}

TEST(FitnessMachineControlPoint, EncodesStopAndPauseWithDistinctControlInformation)
{
    // Same opcode, one parameter octet apart. Swapping them pauses a workout meant to be ended.
    EXPECT_EQ(encodeStop(), (std::vector<uint8_t>{0x08, 0x01}));
    EXPECT_EQ(encodePause(), (std::vector<uint8_t>{0x08, 0x02}));
}

TEST(FitnessMachineControlPoint, EncodesTargetPowerAsLittleEndianSignedWatts)
{
    EXPECT_EQ(encodeSetTargetPower(200), (std::vector<uint8_t>{0x05, 0xC8, 0x00}));
    EXPECT_EQ(encodeSetTargetPower(-50), (std::vector<uint8_t>{0x05, 0xCE, 0xFF}));
}

TEST(FitnessMachineControlPoint, EncodesTargetResistanceLevelInTenths)
{
    // A uint8 in tenths, even though the same quantity is reported as a whole-numbered sint16.
    EXPECT_EQ(encodeSetTargetResistanceLevel(7.5), (std::vector<uint8_t>{0x04, 0x4B}));

    // ...and a value the octet cannot hold is clamped rather than wrapped into something legal
    // but wrong.
    EXPECT_EQ(encodeSetTargetResistanceLevel(100.0), (std::vector<uint8_t>{0x04, 0xFF}));
    EXPECT_EQ(encodeSetTargetResistanceLevel(-4.0), (std::vector<uint8_t>{0x04, 0x00}));
}

TEST(FitnessMachineControlPoint, EncodesSimulationParameters)
{
    // Four fields, four different resolutions: 0.001 m/s, 0.01 %, 0.0001, 0.01 kg/m.
    // 5.00 % -> 500 -> 0x01F4; Crr 0.0040 -> 40 -> 0x28; Cw 0.51 -> 51 -> 0x33.
    EXPECT_EQ(encodeSetSimulationParameters(0.0, 5.0, 0.004, 0.51),
              (std::vector<uint8_t>{0x11, 0x00, 0x00, 0xF4, 0x01, 0x28, 0x33}));
}

TEST(FitnessMachineControlPoint, EncodesADescentAndAHeadwind)
{
    // -3.5 % -> -350 -> 0xFEA2; -2.0 m/s -> -2000 -> 0xF830.
    const auto request = encodeSetSimulationParameters(-2.0, -3.5, 0.004, 0.51);

    EXPECT_EQ(request, (std::vector<uint8_t>{0x11, 0x30, 0xF8, 0xA2, 0xFE, 0x28, 0x33}));
}

TEST(FitnessMachineControlPoint, DecodesAResponseIndication)
{
    const auto response = decodeControlPointResponse({0x80, 0x05, 0x01});

    ASSERT_TRUE(response);
    EXPECT_EQ(response->requestOpCode, 0x05);
    EXPECT_EQ(response->result, 0x01);
    EXPECT_STREQ(controlPointOpCodeName(response->requestOpCode), "Set Target Power");
    EXPECT_STREQ(controlPointResultName(response->result), "Success");
}

TEST(FitnessMachineControlPoint, NamesEveryResultCode)
{
    EXPECT_STREQ(controlPointResultName(0x02), "Op Code Not Supported");
    EXPECT_STREQ(controlPointResultName(0x03), "Invalid Parameter");
    EXPECT_STREQ(controlPointResultName(0x04), "Operation Failed");
    EXPECT_STREQ(controlPointResultName(0x05), "Control Not Permitted");
    EXPECT_STREQ(controlPointResultName(0x42), "Unknown");
}

TEST(FitnessMachineControlPoint, IgnoresAnIndicationThatIsNotAResponse)
{
    // Attributing a stray indication to the last request sent would report someone else's verdict
    // as ours.
    EXPECT_FALSE(decodeControlPointResponse({0x05, 0xC8, 0x00}));
    EXPECT_FALSE(decodeControlPointResponse({}));
    EXPECT_FALSE(decodeControlPointResponse({0x80, 0x05}));
}

TEST(FitnessMachineControlPoint, AcceptsAResponseCarryingAParameter)
{
    // Spin Down returns two target speeds behind the result code. We never send it, but another
    // client on the same link can, and the trailing octets must not make this look malformed.
    const auto response = decodeControlPointResponse({0x80, 0x13, 0x01, 0xB8, 0x0B, 0x10, 0x0E});

    ASSERT_TRUE(response);
    EXPECT_EQ(response->requestOpCode, 0x13);
    EXPECT_EQ(response->result, 0x01);
}

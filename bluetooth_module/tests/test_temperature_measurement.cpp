#include <bluetooth_module/profiles/temperature_measurement.h>

#include <gtest/gtest.h>

#include <string>

using namespace opendaq_bt::profiles;

namespace
{

/*! Flag bits of Temperature Measurement (0x2A1C), spelled out so the tests read as the spec does. */
constexpr uint8_t Fahrenheit = 0x01;
constexpr uint8_t TimeStampPresent = 0x02;
constexpr uint8_t TypePresent = 0x04;

}

// ---- Health Thermometer: Temperature Measurement / Intermediate Temperature --------------------

TEST(TemperatureMeasurement, DecodesCelsius)
{
    // Mantissa 365, exponent -1.
    const auto decoded = decodeTemperatureMeasurement({0x00, 0x6D, 0x01, 0x00, 0xFF});

    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded->celsius);
    EXPECT_DOUBLE_EQ(*decoded->celsius, 36.5);
    EXPECT_FALSE(decoded->temperatureType);
}

TEST(TemperatureMeasurement, ConvertsFahrenheitToCelsius)
{
    // 98.6 degF: mantissa 986, exponent -1.
    const auto decoded = decodeTemperatureMeasurement({Fahrenheit, 0xDA, 0x03, 0x00, 0xFF});

    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded->celsius);
    EXPECT_NEAR(*decoded->celsius, 37.0, 1e-9);
}

TEST(TemperatureMeasurement, SignExtendsTheTwentyFourBitMantissa)
{
    // Mantissa -55 (0xFFFFC9), exponent -1.
    const auto decoded = decodeTemperatureMeasurement({0x00, 0xC9, 0xFF, 0xFF, 0xFF});

    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded->celsius);
    EXPECT_DOUBLE_EQ(*decoded->celsius, -5.5);
}

TEST(TemperatureMeasurement, AppliesAPositiveExponent)
{
    const auto decoded = decodeTemperatureMeasurement({0x00, 0x04, 0x00, 0x00, 0x01});

    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded->celsius);
    EXPECT_DOUBLE_EQ(*decoded->celsius, 40.0);
}

TEST(TemperatureMeasurement, SkipsTheTimeStampToReachTheType)
{
    // Getting the time stamp length wrong still parses, so this pins the type's offset down.
    const auto decoded = decodeTemperatureMeasurement(
        {static_cast<uint8_t>(TimeStampPresent | TypePresent),
         0x6D, 0x01, 0x00, 0xFF,                     // 36.5
         0xEA, 0x07, 0x0A, 0x0E, 0x0C, 0x1E, 0x00,   // 2026-10-14 12:30:00
         0x03});                                     // Ear

    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded->celsius);
    EXPECT_DOUBLE_EQ(*decoded->celsius, 36.5);
    ASSERT_TRUE(decoded->temperatureType);
    EXPECT_EQ(*decoded->temperatureType, 3);
}

TEST(TemperatureMeasurement, DecodesTheTypeWithoutATimeStamp)
{
    const auto decoded = decodeTemperatureMeasurement({TypePresent, 0x6D, 0x01, 0x00, 0xFF, 0x06});

    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded->temperatureType);
    EXPECT_EQ(*decoded->temperatureType, 6);
}

TEST(TemperatureMeasurement, ReportsNoValueForTheSpecialFloats)
{
    // NaN, NRes, +INF, -INF: well-formed frames that carry no reading.
    for (const uint32_t mantissa : {0x007FFFFFu, 0x00800000u, 0x007FFFFEu, 0x00800002u})
    {
        const auto decoded = decodeTemperatureMeasurement(
            {0x00,
             static_cast<uint8_t>(mantissa),
             static_cast<uint8_t>(mantissa >> 8),
             static_cast<uint8_t>(mantissa >> 16),
             0x00});

        ASSERT_TRUE(decoded) << std::hex << mantissa;
        EXPECT_FALSE(decoded->celsius) << std::hex << mantissa;
    }
}

TEST(TemperatureMeasurement, RejectsEmptyPayload)
{
    EXPECT_FALSE(decodeTemperatureMeasurement({}));
}

TEST(TemperatureMeasurement, RejectsFlagsWithoutAValue)
{
    EXPECT_FALSE(decodeTemperatureMeasurement({0x00}));
}

TEST(TemperatureMeasurement, RejectsATruncatedFloat)
{
    EXPECT_FALSE(decodeTemperatureMeasurement({0x00, 0x6D, 0x01, 0x00}));
}

TEST(TemperatureMeasurement, RejectsATruncatedTimeStamp)
{
    EXPECT_FALSE(decodeTemperatureMeasurement({TimeStampPresent, 0x6D, 0x01, 0x00, 0xFF, 0xEA, 0x07}));
}

TEST(TemperatureMeasurement, RejectsATypeFlagWithNoType)
{
    EXPECT_FALSE(decodeTemperatureMeasurement({TypePresent, 0x6D, 0x01, 0x00, 0xFF}));
}

TEST(TemperatureMeasurement, NamesTemperatureTypes)
{
    EXPECT_STREQ(temperatureTypeName(1), "Armpit");
    EXPECT_STREQ(temperatureTypeName(9), "Tympanum");
    EXPECT_STREQ(temperatureTypeName(0), "Unknown");
    EXPECT_STREQ(temperatureTypeName(42), "Unknown");
}

// ---- Environmental Sensing: Temperature --------------------------------------------------------

TEST(EnvironmentalTemperature, DecodesHundredthsOfADegree)
{
    // 2570 * 0.01
    const auto decoded = decodeEnvironmentalTemperature({0x0A, 0x0A});

    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded->celsius);
    EXPECT_DOUBLE_EQ(*decoded->celsius, 25.70);
}

TEST(EnvironmentalTemperature, DecodesNegativeValues)
{
    // -1050 * 0.01
    const auto decoded = decodeEnvironmentalTemperature({0xE6, 0xFB});

    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded->celsius);
    EXPECT_DOUBLE_EQ(*decoded->celsius, -10.5);
}

TEST(EnvironmentalTemperature, ReportsNoValueForTheUnknownSentinel)
{
    const auto decoded = decodeEnvironmentalTemperature({0x00, 0x80});

    ASSERT_TRUE(decoded);
    EXPECT_FALSE(decoded->celsius);
}

TEST(EnvironmentalTemperature, RejectsAnythingButTwoOctets)
{
    EXPECT_FALSE(decodeEnvironmentalTemperature({}));
    EXPECT_FALSE(decodeEnvironmentalTemperature({0x0A}));
    EXPECT_FALSE(decodeEnvironmentalTemperature({0x0A, 0x0A, 0x00}));
}

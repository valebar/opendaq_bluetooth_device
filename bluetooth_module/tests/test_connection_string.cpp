#include <bluetooth_module/connection_string.h>

#include <gtest/gtest.h>

using namespace opendaq_bt;

TEST(ConnectionString, FormatsMacAddress)
{
    EXPECT_EQ(formatConnectionString("AA:BB:CC:DD:EE:FF"), "bluetooth://AA:BB:CC:DD:EE:FF");
}

TEST(ConnectionString, NormalisesSpellingsOfTheSameMac)
{
    // Linux prints colons, Windows tooling often prints dashes, and casing varies everywhere.
    // All three have to converge, or the same peripheral would appear as several devices.
    const std::string canonical = "AA:BB:CC:DD:EE:FF";

    EXPECT_EQ(normalizeIdentifier("aa:bb:cc:dd:ee:ff"), canonical);
    EXPECT_EQ(normalizeIdentifier("AA-BB-CC-DD-EE-FF"), canonical);
    EXPECT_EQ(normalizeIdentifier("aabbccddeeff"), canonical);
    EXPECT_EQ(normalizeIdentifier("  AA:bb:CC:dd:EE:ff  "), canonical);
}

TEST(ConnectionString, NormalisesCoreBluetoothUuid)
{
    const std::string canonical = "E4C7A1B2-3D45-4F6A-8B9C-0D1E2F3A4B5C";

    EXPECT_EQ(normalizeIdentifier("e4c7a1b2-3d45-4f6a-8b9c-0d1e2f3a4b5c"), canonical);
    EXPECT_EQ(normalizeIdentifier("e4c7a1b23d454f6a8b9c0d1e2f3a4b5c"), canonical);
}

TEST(ConnectionString, RejectsIdentifiersThatAreNeitherMacNorUuid)
{
    EXPECT_TRUE(normalizeIdentifier("").empty());
    EXPECT_TRUE(normalizeIdentifier("AA:BB:CC").empty());
    EXPECT_TRUE(normalizeIdentifier("not-hex-at-all").empty());
    EXPECT_TRUE(normalizeIdentifier("AA:BB:CC:DD:EE:FF:00").empty());
}

TEST(ConnectionString, ParsesItsOwnOutput)
{
    std::string identifier;
    std::string error;

    ASSERT_TRUE(tryParseConnectionString(formatConnectionString("aa:bb:cc:dd:ee:ff"), identifier, error))
        << error;
    EXPECT_EQ(identifier, "AA:BB:CC:DD:EE:FF");
}

TEST(ConnectionString, AcceptsCaseInsensitiveScheme)
{
    std::string identifier;
    std::string error;

    EXPECT_TRUE(tryParseConnectionString("BLUETOOTH://AA:BB:CC:DD:EE:FF", identifier, error));
    EXPECT_EQ(identifier, "AA:BB:CC:DD:EE:FF");
}

TEST(ConnectionString, RejectsForeignSchemes)
{
    std::string identifier;
    std::string error;

    EXPECT_FALSE(tryParseConnectionString("daq.opcua://192.168.0.1", identifier, error));
    EXPECT_NE(error.find("bluetooth://"), std::string::npos) << error;
}

TEST(ConnectionString, RejectsEmptyIdentifier)
{
    std::string identifier;
    std::string error;

    EXPECT_FALSE(tryParseConnectionString("bluetooth://", identifier, error));
    EXPECT_FALSE(error.empty());
}

TEST(ConnectionString, ErrorNamesBothAcceptedIdentifierForms)
{
    std::string identifier;
    std::string error;

    ASSERT_FALSE(tryParseConnectionString("bluetooth://nonsense", identifier, error));
    EXPECT_NE(error.find("MAC address"), std::string::npos) << error;
    EXPECT_NE(error.find("UUID"), std::string::npos) << error;
}

TEST(ConnectionString, BuildsLocalIdWithoutPunctuation)
{
    EXPECT_EQ(identifierToLocalId("AA:BB:CC:DD:EE:FF"), "BLE_AA_BB_CC_DD_EE_FF");
    EXPECT_EQ(identifierToLocalId("E4C7A1B2-3D45-4F6A-8B9C-0D1E2F3A4B5C"),
              "BLE_E4C7A1B2_3D45_4F6A_8B9C_0D1E2F3A4B5C");
}

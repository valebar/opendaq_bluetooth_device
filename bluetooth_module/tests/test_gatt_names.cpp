#include <bluetooth_module/ble/ble_types.h>
#include <bluetooth_module/gatt_names.h>

#include <gtest/gtest.h>

using namespace opendaq_bt;

TEST(GattNames, ExpandsShorthandUuids)
{
    EXPECT_EQ(ble::expandUuid("180F"), "0000180f-0000-1000-8000-00805f9b34fb");
    EXPECT_EQ(ble::expandUuid("0000180f-0000-1000-8000-00805f9b34fb"),
              "0000180f-0000-1000-8000-00805f9b34fb");
}

TEST(GattNames, ResolvesAssignedNumbersThroughEitherSpelling)
{
    EXPECT_EQ(wellKnownGattName("180F"), "Battery Service");
    EXPECT_EQ(wellKnownGattName("0000180F-0000-1000-8000-00805F9B34FB"), "Battery Service");
    EXPECT_EQ(wellKnownGattName("6e400001-b5a3-f393-e0a9-e50e24dcca9e"), "Nordic UART");
}

TEST(GattNames, NamesTheCyclingAndFitnessMachineEntries)
{
    EXPECT_EQ(wellKnownGattName("1818"), "Cycling Power");
    EXPECT_EQ(wellKnownGattName("1826"), "Fitness Machine");
    EXPECT_EQ(wellKnownGattName("2A63"), "Cycling Power Measurement");
    EXPECT_EQ(wellKnownGattName("2AD2"), "Indoor Bike Data");
    EXPECT_EQ(wellKnownGattName("2AD9"), "Fitness Machine Control Point");

    // Named but not decoded: a treadmill hosts the same service, and browsing one readably is
    // what tells a user why it produced no channels.
    EXPECT_EQ(wellKnownGattName("2ACD"), "Treadmill Data");
}

TEST(GattNames, ReturnsEmptyForVendorSpecificUuids)
{
    EXPECT_TRUE(wellKnownGattName("12345678-1234-1234-1234-123456789abc").empty());
}

TEST(GattNames, ShortensSigUuidsButKeepsVendorOnesDistinct)
{
    EXPECT_EQ(shortUuid("0000180f-0000-1000-8000-00805f9b34fb"), "180F");
    EXPECT_EQ(shortUuid("6e400001-b5a3-f393-e0a9-e50e24dcca9e"), "6E400001");
}

TEST(GattNames, RendersPropertyFlags)
{
    EXPECT_EQ(ble::characteristicPropertiesToString(ble::CharPropRead | ble::CharPropNotify),
              "read,notify");
    EXPECT_EQ(ble::characteristicPropertiesToString(ble::CharPropNone), "");
}

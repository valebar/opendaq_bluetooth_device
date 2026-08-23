#include "fake_ble_central.h"

#include <bluetooth_module/scan_cache.h>

#include <gtest/gtest.h>

#include <chrono>

namespace ble = opendaq_bt::ble;

using opendaq_bt::ScanCache;
using opendaq_bt::testing::FakeCentral;
using namespace std::chrono_literals;

namespace
{

ble::Advertisement heard(const std::string& id, std::chrono::steady_clock::time_point lastSeen)
{
    ble::Advertisement result;
    result.id = id;
    result.rssi = -50;
    result.lastSeen = lastSeen;
    return result;
}

ble::Advertisement linked(const std::string& id)
{
    // What a backend reports for a peripheral this host is connected to: no reading, and a
    // sighting from whenever it last advertised, which was before it connected.
    auto result = heard(id, std::chrono::steady_clock::now() - 5min);
    result.rssi = 0;
    result.connected = true;
    return result;
}

// Warm-up is zero throughout: the production default blocks the first snapshot for two seconds,
// which would cost every test in this file that long for nothing.
std::unique_ptr<ScanCache> cacheOver(FakeCentral& central,
                                     std::chrono::milliseconds deviceTimeout = 30s)
{
    return std::make_unique<ScanCache>(central, deviceTimeout, 0ms);
}

}

TEST(ScanCache, StartsTheScanOnceHoweverManySnapshotsAreTaken)
{
    FakeCentral central;
    const auto cache = cacheOver(central);

    cache->snapshot();
    cache->snapshot();

    EXPECT_EQ(central.scans(), 1);
    EXPECT_EQ(central.discoveries(), 2);
}

TEST(ScanCache, DropsEntriesNotSeenWithinTheDeviceTimeout)
{
    FakeCentral central;
    central.addAdvertisement(heard("AA:BB:CC:DD:EE:FF", std::chrono::steady_clock::now() - 5min));
    central.addAdvertisement(heard("11:22:33:44:55:66", std::chrono::steady_clock::now()));

    const auto cache = cacheOver(central);

    const auto snapshot = cache->snapshot();
    ASSERT_EQ(snapshot.size(), 1u);
    EXPECT_EQ(snapshot[0].id, "11:22:33:44:55:66");
}

TEST(ScanCache, KeepsAConnectedPeripheralHoweverLongAgoItLastAdvertised)
{
    FakeCentral central;
    central.addAdvertisement(linked("02:00:00:00:00:01"));

    const auto cache = cacheOver(central);

    // The link is the evidence that the peripheral is there. Timing it out on a sighting that can
    // only ever get older would drop the one device the module is certain it can reach.
    const auto snapshot = cache->snapshot();
    ASSERT_EQ(snapshot.size(), 1u);
    EXPECT_TRUE(snapshot[0].connected);
}

TEST(ScanCache, DropsAConnectedPeripheralOnceTheBackendStopsReportingIt)
{
    FakeCentral central;
    central.addAdvertisement(linked("02:00:00:00:00:01"));

    const auto cache = cacheOver(central);
    ASSERT_EQ(cache->snapshot().size(), 1u);

    // A backend reports a connected peripheral only while the link is up, so exempting it from the
    // timeout cannot strand it: it leaves on the first snapshot after the link drops.
    central.setAdvertisements({});
    EXPECT_TRUE(cache->snapshot().empty());
}

TEST(ScanCache, KeepsEveryEntryWhenTheTimeoutIsDisabled)
{
    FakeCentral central;
    central.addAdvertisement(heard("AA:BB:CC:DD:EE:FF", std::chrono::steady_clock::now() - 5min));

    const auto cache = cacheOver(central, 0ms);

    EXPECT_EQ(cache->snapshot().size(), 1u);
}

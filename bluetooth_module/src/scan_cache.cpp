#include <bluetooth_module/scan_cache.h>

#include <algorithm>
#include <thread>

namespace opendaq_bt
{

ScanCache::ScanCache(ble::ICentral& central,
                     std::chrono::milliseconds deviceTimeout,
                     std::chrono::milliseconds warmup)
    : central(central)
    , deviceTimeout(deviceTimeout)
    , warmup(warmup)
{
}

ScanCache::~ScanCache()
{
    stop();
}

std::vector<ble::Advertisement> ScanCache::snapshot()
{
    bool justStarted = false;

    {
        std::scoped_lock lock(sync);
        if (!scanStarted)
        {
            central.startScan();
            scanStarted = true;
            justStarted = true;
        }
    }

    // Block only on the very first call, and only long enough for the radio to report something.
    // Every later call reads whatever the background scan has accumulated since.
    if (justStarted && warmup.count() > 0)
        std::this_thread::sleep_for(warmup);

    auto discovered = central.getDiscovered();

    if (deviceTimeout.count() > 0)
    {
        const auto now = std::chrono::steady_clock::now();
        const auto stale = [&](const ble::Advertisement& advertisement) {
            // A peripheral this host is connected to has stopped advertising, so its last sighting
            // only gets older and the timeout would eventually drop the one device we are sure of.
            // It leaves the list when the backend stops reporting it, which is the first snapshot
            // after the link drops.
            if (advertisement.connected)
                return false;

            return now - advertisement.lastSeen > deviceTimeout;
        };

        discovered.erase(std::remove_if(discovered.begin(), discovered.end(), stale),
                         discovered.end());
    }

    // Deliberately not reordered. The backend's own order is stable (both keep peripherals in a
    // std::map, so identifier-ascending), and openDAQ preserves whatever a module hands back --
    // its device Dict is insertion-ordered. Sorting by RSSI here, as this used to, reshuffled the
    // list on every getAvailableDevices(): peripherals advertise several times a second and each
    // packet refreshes the reading, so a few dBm of normal fluctuation swaps neighbours. Clients
    // that want signal strength can sort on the "Rssi" property carried by every DeviceInfo.
    return discovered;
}

void ScanCache::stop()
{
    std::scoped_lock lock(sync);
    if (!scanStarted)
        return;

    central.stopScan();
    scanStarted = false;
}

}

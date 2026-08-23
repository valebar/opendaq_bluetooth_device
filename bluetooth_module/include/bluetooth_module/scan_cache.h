#pragma once

#include <bluetooth_module/ble/ble_central.h>

#include <chrono>
#include <mutex>
#include <vector>

namespace opendaq_bt
{

/*!
 * Bridges BLE scanning, which is inherently slow and continuous, to openDAQ's device discovery,
 * which is a synchronous call expected to return promptly.
 *
 * The backend scan is started lazily on the first snapshot() and then left running, accumulating
 * peripherals in the background. Every subsequent snapshot() is a non-blocking read of what has
 * been seen so far, minus entries that have gone stale.
 *
 * Only the very first snapshot() blocks, for a bounded warm-up period, so that an application
 * calling getAvailableDevices() once immediately after startup does not get a misleadingly empty
 * list before the radio has had a chance to hear anything.
 */
class ScanCache
{
public:
    ScanCache(ble::ICentral& central,
              std::chrono::milliseconds deviceTimeout,
              std::chrono::milliseconds warmup);
    ~ScanCache();

    ScanCache(const ScanCache&) = delete;
    ScanCache& operator=(const ScanCache&) = delete;

    /*! Currently-visible peripherals, freshest data the backend has. */
    std::vector<ble::Advertisement> snapshot();

    /*! Stops the underlying scan. Safe to call when no scan is running. */
    void stop();

private:
    ble::ICentral& central;
    const std::chrono::milliseconds deviceTimeout;
    const std::chrono::milliseconds warmup;

    std::mutex sync;
    bool scanStarted = false;
};

}

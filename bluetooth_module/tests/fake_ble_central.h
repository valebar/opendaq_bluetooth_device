#pragma once

#include <bluetooth_module/ble/ble_central.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <condition_variable>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace opendaq_bt::testing
{

/*!
 * A scripted BLE backend. Everything the openDAQ layer does -- discovery, connection-string
 * routing, GATT tree construction, characteristic reads and writes -- is driven through the
 * ICentral interface, so substituting this makes the entire module testable with no radio,
 * no adapter and no BlueZ, identically on every platform.
 */
class FakeConnection final : public ble::IConnection
{
public:
    FakeConnection(std::vector<ble::Service> services,
                   std::map<std::string, std::vector<uint8_t>> values,
                   uint16_t mtu,
                   std::shared_ptr<bool> destroyedFlag)
        : resolvedServices(std::move(services))
        , values(std::move(values))
        , destroyed(std::move(destroyedFlag))
        , mtuSize(mtu)
    {
    }

    ~FakeConnection() override
    {
        // A real BlueZ connection joins its own D-Bus dispatch thread as it goes, and the
        // disconnect callback is delivered on that very thread -- so destroying a connection from
        // inside its own disconnect notification joins the calling thread to itself. The fake has
        // no dispatch thread to deadlock on, so the mistake would otherwise pass here and only
        // hang on hardware.
        const auto disconnecting = disconnectingThread->load();
        if (disconnecting != 0)
        {
            EXPECT_NE(disconnecting, std::hash<std::thread::id>{}(std::this_thread::get_id()))
                << "A connection was destroyed from inside its own disconnect callback; on Linux "
                   "that joins the D-Bus dispatch thread to itself.";
        }

        if (destroyed)
            *destroyed = true;
    }

    bool isConnected() const override { return connected; }
    const std::vector<ble::Service>& services() const override { return resolvedServices; }
    uint16_t mtu() const override { return mtuSize; }

    std::vector<uint8_t> read(const std::string& handle) override
    {
        std::scoped_lock lock(sync);

        // A dead link fails its operations; without this a test cannot tell a working connection
        // from one the peripheral walked away from.
        if (!connected)
            throw ble::BleError("Device is disconnected.");

        readThreadIds.push_back(std::this_thread::get_id());

        if (failingHandles.count(handle) != 0)
            throw ble::BleError("Read not permitted");

        const auto it = values.find(handle);
        if (it == values.end())
            throw ble::BleError("No such characteristic: " + handle);

        ++readCount;
        return it->second;
    }

    void write(const std::string& handle, const std::vector<uint8_t>& data, bool withResponse) override
    {
        std::scoped_lock lock(sync);

        if (!connected)
            throw ble::BleError("Device is disconnected.");

        values[handle] = data;
        writes.emplace_back(handle, data);
        lastWriteWithResponse = withResponse;
    }

    void subscribe(const std::string& handle, ble::NotificationCallback callback) override
    {
        std::scoped_lock lock(sync);

        if (!connected)
            throw ble::BleError("Device is disconnected.");

        if (refusedSubscriptions.count(handle) != 0)
            throw ble::BleError("Notifications are not permitted on " + handle);

        notificationCallbacks[handle] = std::move(callback);
    }

    void unsubscribe(const std::string& handle) override
    {
        std::scoped_lock lock(sync);
        notificationCallbacks.erase(handle);
    }

    void setDisconnectCallback(std::function<void()> callback) override
    {
        std::scoped_lock lock(sync);
        disconnectCallback = std::move(callback);
    }

    /*!
     * Delivers a notification as if the peripheral had sent it.
     *
     * Called synchronously on the test's own thread, which is what lets a test push a payload and
     * immediately assert on the packets it produced -- no waiting, no flakiness.
     */
    void pushNotification(const std::string& handle, const std::vector<uint8_t>& value)
    {
        ble::NotificationCallback callback;
        {
            std::scoped_lock lock(sync);
            const auto it = notificationCallbacks.find(handle);
            if (it == notificationCallbacks.end())
                throw ble::BleError("Nothing is subscribed to " + handle);
            callback = it->second;
        }

        callback(value);
    }

    bool isSubscribed(const std::string& handle) const
    {
        std::scoped_lock lock(sync);
        return notificationCallbacks.count(handle) != 0;
    }

    /*! Simulates the peripheral dropping the link. */
    void simulateDisconnect()
    {
        // Copied out before the callback runs: the reconnector destroys this connection in
        // response, so nothing may touch the object once the callback has been invoked.
        const auto marker = disconnectingThread;

        std::function<void()> callback;
        {
            std::scoped_lock lock(sync);
            connected = false;
            marker->store(std::hash<std::thread::id>{}(std::this_thread::get_id()));
            callback = disconnectCallback;
        }

        if (callback)
            callback();

        marker->store(0);
    }

    /*! Makes subscribing to @p handle fail, as a peripheral that lost its bond does. */
    void refuseSubscriptionsFor(const std::string& handle)
    {
        std::scoped_lock lock(sync);
        refusedSubscriptions.insert(handle);
    }

    /*! Drops the link without telling anyone, as a peripheral that simply vanished does. */
    void goSilent()
    {
        std::scoped_lock lock(sync);
        connected = false;
    }

    void failReadsFor(const std::string& handle)
    {
        std::scoped_lock lock(sync);
        failingHandles.insert(handle);
    }

    /*! Changes what the peripheral returns for subsequent reads of @p handle. */
    void setValue(const std::string& handle, std::vector<uint8_t> value)
    {
        std::scoped_lock lock(sync);
        values[handle] = std::move(value);
    }

    std::vector<std::pair<std::string, std::vector<uint8_t>>> takeWrites()
    {
        std::scoped_lock lock(sync);
        return writes;
    }

    /*!
     * Which threads have read from this connection.
     *
     * openDAQ holds the device's config lock across a property read handler, and a backend thread
     * holds the connection's bus mutex; a handler that read from the radio would take the two in
     * the opposite order and deadlock on real hardware. A fake has no bus mutex to deadlock on, so
     * a test asserts the ordering rule through this instead.
     */
    std::vector<std::thread::id> readThreads() const
    {
        std::scoped_lock lock(sync);
        return readThreadIds;
    }

    bool lastWriteHadResponse() const { return lastWriteWithResponse; }
    int reads() const { return readCount; }

private:
    mutable std::mutex sync;
    std::vector<ble::Service> resolvedServices;
    std::shared_ptr<bool> destroyed;
    std::map<std::string, std::vector<uint8_t>> values;
    std::set<std::string> failingHandles;
    std::set<std::string> refusedSubscriptions;
    std::vector<std::thread::id> readThreadIds;

    /*! Held indirectly so it outlives the connection the disconnect callback destroys. */
    std::shared_ptr<std::atomic<size_t>> disconnectingThread =
        std::make_shared<std::atomic<size_t>>(0);
    std::vector<std::pair<std::string, std::vector<uint8_t>>> writes;
    std::function<void()> disconnectCallback;
    std::map<std::string, ble::NotificationCallback> notificationCallbacks;
    uint16_t mtuSize;
    bool connected = true;
    bool lastWriteWithResponse = false;
    int readCount = 0;
};

class FakeCentral final : public ble::ICentral
{
public:
    bool isAvailable(std::string& reason) override
    {
        if (!available)
            reason = unavailableReason;
        return available;
    }

    void startScan() override { scanning = true; ++scanStarts; }
    void stopScan() override { scanning = false; }
    bool isScanning() const override { return scanning; }

    void setServicesOfInterest(const std::vector<std::string>& uuids) override
    {
        std::scoped_lock lock(sync);
        servicesOfInterest = uuids;
    }

    std::vector<ble::Advertisement> getDiscovered() override
    {
        std::scoped_lock lock(sync);
        ++discoveryCalls;
        return advertisements;
    }

    std::unique_ptr<ble::IConnection> connect(const std::string& id,
                                              std::chrono::milliseconds) override
    {
        std::unique_lock lock(sync);

        lastConnectId = id;
        ++attempts;

        // Whether the connection handed out by the previous attempt was already gone when this one
        // started. A BlueZ connection disconnects the peripheral as it is destroyed, and the object
        // path is derived from the MAC -- so a stale one destroyed after a reconnect tears the new
        // link straight back down.
        previousDestroyed.push_back(!connectionDestroyed || *connectionDestroyed);

        attemptMade.notify_all();

        if (unreachable.count(id) != 0)
            throw ble::BleError("Device " + id + " is not reachable");

        const auto it = connectables.find(id);
        if (it == connectables.end())
            throw ble::BleError("Device " + id + " is not reachable");

        connectionDestroyed = std::make_shared<bool>(false);

        auto connection = std::make_unique<FakeConnection>(
            it->second.services, it->second.values, it->second.mtu, connectionDestroyed);

        for (const auto& handle : refusedSubscriptions)
            connection->refuseSubscriptionsFor(handle);

        lastConnection = connection.get();
        allConnections.push_back(lastConnection);

        return connection;
    }

    // ---- test scripting -------------------------------------------------------------------

    void addAdvertisement(ble::Advertisement advertisement)
    {
        std::scoped_lock lock(sync);
        advertisements.push_back(std::move(advertisement));
    }

    /*! Replaces the scripted list, so a test can take a peripheral away mid-run. */
    void setAdvertisements(std::vector<ble::Advertisement> replacement)
    {
        std::scoped_lock lock(sync);
        advertisements = std::move(replacement);
    }


    struct Peripheral
    {
        std::vector<ble::Service> services;
        std::map<std::string, std::vector<uint8_t>> values;
        uint16_t mtu = 247;
    };

    /*! Takes a peripheral out of range, and puts it back, without discarding its scripted GATT. */
    void setReachable(const std::string& id, bool reachable)
    {
        std::scoped_lock lock(sync);
        if (reachable)
            unreachable.erase(id);
        else
            unreachable.insert(id);
    }

    /*! Makes every connection handed out from now on refuse notifications on @p handle. */
    void refuseSubscriptionsFor(const std::string& handle)
    {
        std::scoped_lock lock(sync);
        refusedSubscriptions.insert(handle);
    }

    int connectAttempts() const
    {
        std::scoped_lock lock(sync);
        return attempts;
    }

    /*! Every connection handed out, oldest first. connection() stays "the latest". */
    FakeConnection* connectionAt(size_t index) const
    {
        std::scoped_lock lock(sync);
        return index < allConnections.size() ? allConnections[index] : nullptr;
    }

    size_t connectionCount() const
    {
        std::scoped_lock lock(sync);
        return allConnections.size();
    }

    /*! Whether attempt @p index began with the previous connection already destroyed. */
    bool previousWasDestroyedAt(size_t index) const
    {
        std::scoped_lock lock(sync);
        return index < previousDestroyed.size() && previousDestroyed[index];
    }

    /*!
     * Blocks until connect() has been entered @p count times, or the timeout expires.
     *
     * Condition-variable based, so a test that is going to pass returns the instant the attempt
     * lands; the timeout only bounds a genuine failure rather than pacing a passing run.
     */
    bool waitForConnectAttempts(int count, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(sync);
        return attemptMade.wait_for(lock, timeout, [this, count] { return attempts >= count; });
    }

    void addPeripheral(const std::string& id, Peripheral peripheral)
    {
        std::scoped_lock lock(sync);
        connectables[id] = std::move(peripheral);
    }

    void setUnavailable(const std::string& reason)
    {
        available = false;
        unavailableReason = reason;
    }

    FakeConnection* connection() const { return lastConnection; }

    /*!
     * Flag set when the connection handed out by the last connect() is destroyed. A device that
     * leaks -- for instance through a property value holding a reference back to its own
     * component -- never releases it, so this is how a refcount cycle becomes observable.
     *
     * Returned as a shared_ptr on purpose: the module owns this backend, so a test that waits
     * until the module is gone before checking would be reading freed memory. Take the flag
     * first, then let the module go.
     */
    std::shared_ptr<bool> connectionDestroyedFlag() const
    {
        std::scoped_lock lock(sync);
        return connectionDestroyed;
    }
    std::string connectedId() const { return lastConnectId; }
    int scans() const { return scanStarts; }
    int discoveries() const { return discoveryCalls; }

    std::vector<std::string> interestingServices() const
    {
        std::scoped_lock lock(sync);
        return servicesOfInterest;
    }

private:
    mutable std::mutex sync;
    std::vector<ble::Advertisement> advertisements;

    // What the module told us it has profiles for. Only CoreBluetooth needs it in production;
    // here it exists so a test can check the module hands the registry across the seam at all.
    std::vector<std::string> servicesOfInterest;
    std::map<std::string, Peripheral> connectables;
    std::set<std::string> unreachable;
    std::set<std::string> refusedSubscriptions;
    std::vector<std::thread::id> readThreadIds;
    std::vector<FakeConnection*> allConnections;
    std::vector<bool> previousDestroyed;
    mutable std::condition_variable attemptMade;
    int attempts = 0;
    std::string lastConnectId;
    std::string unavailableReason;
    FakeConnection* lastConnection = nullptr;
    std::shared_ptr<bool> connectionDestroyed;
    bool available = true;
    bool scanning = false;
    int scanStarts = 0;
    int discoveryCalls = 0;
};

}

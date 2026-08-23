#include <bluetooth_module/reconnecting_connection.h>

#include <cstddef>
#include <tuple>
#include <utility>

namespace opendaq_bt
{

namespace
{

/*!
 * Identity of a characteristic that does not depend on its backend handle: which service it is in,
 * which UUID it has, and -- because a peripheral may legitimately expose the same characteristic
 * twice -- how many like it came before.
 */
using CharacteristicKey = std::tuple<std::string, std::size_t, std::string, std::size_t>;

std::map<CharacteristicKey, std::string> indexHandles(const std::vector<ble::Service>& services)
{
    std::map<CharacteristicKey, std::string> index;
    std::map<std::string, std::size_t> servicesSeen;

    for (const auto& service : services)
    {
        const auto serviceUuid = ble::expandUuid(service.uuid);
        const auto serviceOrdinal = servicesSeen[serviceUuid]++;

        std::map<std::string, std::size_t> characteristicsSeen;

        for (const auto& characteristic : service.characteristics)
        {
            const auto uuid = ble::expandUuid(characteristic.uuid);
            const auto ordinal = characteristicsSeen[uuid]++;

            index[CharacteristicKey(serviceUuid, serviceOrdinal, uuid, ordinal)] = characteristic.handle;
        }
    }

    return index;
}

/*!
 * Maps every frozen handle onto the handle the newly resolved database uses for the same
 * characteristic. A characteristic the peripheral no longer exposes gets no entry, and operations
 * on it then fail loudly rather than landing somewhere else.
 */
std::map<std::string, std::string> buildRemap(const std::vector<ble::Service>& frozen,
                                              const std::vector<ble::Service>& live)
{
    const auto frozenIndex = indexHandles(frozen);
    const auto liveIndex = indexHandles(live);

    std::map<std::string, std::string> remap;

    for (const auto& [key, handle] : frozenIndex)
    {
        const auto it = liveIndex.find(key);
        if (it != liveIndex.end())
            remap[handle] = it->second;
    }

    return remap;
}

}

struct ReconnectingConnection::Shared : public std::enable_shared_from_this<Shared>
{
    Shared(std::shared_ptr<ble::ICentral> central,
           std::string identifier,
           std::vector<ble::Service> frozenServices,
           Policy policy)
        : central(std::move(central))
        , identifier(std::move(identifier))
        , policy(policy)
        , frozenServices(std::move(frozenServices))
    {
    }

    void supervise();
    bool reestablish(std::string& error, std::string& note);
    void publish(LinkState state, const std::string& message);
    void onInnerDisconnect();

    std::shared_ptr<ble::IConnection> resolve(const std::string& handle, std::string& liveHandle) const;

    const std::shared_ptr<ble::ICentral> central;
    const std::string identifier;
    const Policy policy;
    const std::vector<ble::Service> frozenServices;

    mutable std::mutex sync;
    std::condition_variable wake;

    /*!
     * Shared rather than owned outright so an in-flight read on a thread of openDAQ's keeps the
     * backend connection alive until it returns, even as the supervisor swaps in a new one.
     */
    std::shared_ptr<ble::IConnection> inner;

    /*! Frozen handle -> the handle the live connection knows it by. Identity in the usual case. */
    std::map<std::string, std::string> handleRemap;

    std::map<std::string, ble::NotificationCallback> subscriptions;
    std::function<void()> disconnectCallback;
    LinkStateCallback linkStateCallback;

    uint16_t mtuSize = 0;
    bool linkDown = false;
    bool stopping = false;
};

ReconnectingConnection::ReconnectingConnection(std::shared_ptr<ble::ICentral> central,
                                               std::string identifier,
                                               std::unique_ptr<ble::IConnection> initial,
                                               Policy policy)
    : shared(initial != nullptr
                 ? std::make_shared<Shared>(std::move(central),
                                            std::move(identifier),
                                            initial->services(),
                                            policy)
                 : nullptr)
{
    if (!initial)
        throw ble::BleError("ReconnectingConnection needs an open connection to start from.");

    shared->mtuSize = initial->mtu();
    shared->handleRemap = buildRemap(shared->frozenServices, shared->frozenServices);
    shared->inner = std::move(initial);

    auto state = shared;
    shared->inner->setDisconnectCallback([state] { state->onInnerDisconnect(); });

    supervisor = std::thread([state] { state->supervise(); });
}

ReconnectingConnection::~ReconnectingConnection()
{
    shutdown();
}

void ReconnectingConnection::shutdown()
{
    {
        std::scoped_lock lock(shared->sync);
        if (!shared->stopping)
        {
            shared->stopping = true;

            // Cleared before the join, so a state change already being published is the last one.
            shared->linkStateCallback = nullptr;
        }
    }

    shared->wake.notify_all();

    if (supervisor.joinable())
    {
        // A link state handler holds a reference to the openDAQ device while it runs, so releasing
        // it can be what destroys the device, its channels and this object -- on the supervisor's
        // own thread. Joining then would be joining this thread to itself; detaching is safe
        // because the supervisor holds its own reference to the shared state.
        if (supervisor.get_id() == std::this_thread::get_id())
            supervisor.detach();
        else
            supervisor.join();
    }

    std::shared_ptr<ble::IConnection> connection;
    {
        std::scoped_lock lock(shared->sync);
        shared->disconnectCallback = nullptr;
        connection = shared->inner;
    }

    // Drop the backend callback so a late notification cannot re-enter a half-destroyed object.
    // The connection itself stays open: channels outlive the device's destructor body and
    // unsubscribe through this object as they go.
    if (connection)
        connection->setDisconnectCallback(nullptr);
}

bool ReconnectingConnection::isConnected() const
{
    std::scoped_lock lock(shared->sync);

    // Deliberately our own flag rather than inner->isConnected(): the BlueZ backend keeps that in a
    // bare bool written from its dispatch thread, whereas linkDown is set under this lock.
    return !shared->linkDown && shared->inner != nullptr;
}

const std::vector<ble::Service>& ReconnectingConnection::services() const
{
    return shared->frozenServices;
}

uint16_t ReconnectingConnection::mtu() const
{
    std::scoped_lock lock(shared->sync);
    return shared->mtuSize;
}

std::shared_ptr<ble::IConnection> ReconnectingConnection::Shared::resolve(const std::string& handle,
                                                                          std::string& liveHandle) const
{
    std::scoped_lock lock(sync);

    if (linkDown || !inner)
        throw ble::BleError("Bluetooth device " + identifier + " is disconnected; reconnecting.");

    const auto it = handleRemap.find(handle);
    if (it == handleRemap.end())
        throw ble::BleError("Characteristic " + handle + " is no longer exposed by " + identifier + ".");

    liveHandle = it->second;
    return inner;
}

std::vector<uint8_t> ReconnectingConnection::read(const std::string& handle)
{
    std::string liveHandle;
    const auto connection = shared->resolve(handle, liveHandle);

    // Called with the lock released: a GATT read is a round trip to the peripheral, and holding the
    // lock across it would stall the supervisor's swap for as long as the radio takes.
    return connection->read(liveHandle);
}

void ReconnectingConnection::write(const std::string& handle,
                                   const std::vector<uint8_t>& data,
                                   bool withResponse)
{
    std::string liveHandle;
    const auto connection = shared->resolve(handle, liveHandle);

    connection->write(liveHandle, data, withResponse);
}

void ReconnectingConnection::subscribe(const std::string& handle, ble::NotificationCallback callback)
{
    std::shared_ptr<ble::IConnection> connection;
    std::string liveHandle;

    {
        std::scoped_lock lock(shared->sync);

        // Checked whatever the link state: a handle the peripheral no longer exposes must be
        // refused now, not recorded and then failed on every reconnect for the rest of time.
        const auto it = shared->handleRemap.find(handle);
        if (it == shared->handleRemap.end())
            throw ble::BleError("Characteristic " + handle + " is no longer exposed by " +
                                shared->identifier + ".");

        shared->subscriptions[handle] = callback;

        if (shared->linkDown || !shared->inner)
            return;

        liveHandle = it->second;
        connection = shared->inner;
    }

    try
    {
        connection->subscribe(liveHandle, std::move(callback));
    }
    catch (...)
    {
        // A characteristic that refuses notifications must not be retried forever on every
        // reconnect, so the record only survives a subscription that actually took.
        std::scoped_lock lock(shared->sync);
        shared->subscriptions.erase(handle);
        throw;
    }
}

void ReconnectingConnection::unsubscribe(const std::string& handle)
{
    std::shared_ptr<ble::IConnection> connection;
    std::string liveHandle;

    {
        std::scoped_lock lock(shared->sync);

        shared->subscriptions.erase(handle);

        if (shared->linkDown || !shared->inner)
            return;

        const auto it = shared->handleRemap.find(handle);
        if (it == shared->handleRemap.end())
            return;

        liveHandle = it->second;
        connection = shared->inner;
    }

    connection->unsubscribe(liveHandle);
}

void ReconnectingConnection::setDisconnectCallback(std::function<void()> callback)
{
    std::scoped_lock lock(shared->sync);
    shared->disconnectCallback = std::move(callback);
}

void ReconnectingConnection::setLinkStateCallback(LinkStateCallback callback)
{
    std::scoped_lock lock(shared->sync);
    shared->linkStateCallback = std::move(callback);
}

void ReconnectingConnection::Shared::onInnerDisconnect()
{
    std::function<void()> callback;

    {
        std::scoped_lock lock(sync);
        if (linkDown || stopping)
            return;

        linkDown = true;
        callback = disconnectCallback;
    }

    // This runs on a backend thread -- the D-Bus dispatch thread on Linux -- which is the one
    // thread that must not tear the connection down: doing so joins itself. Flag it and wake the
    // supervisor, which does all the real work.
    wake.notify_all();

    // The plain disconnect callback is invoked here, synchronously, so a caller can treat it as
    // meaning the state has already changed -- there is no window in which the device still claims
    // to be connected while every read already throws.
    //
    // The richer link state goes out from the supervisor instead. On Linux this thread is inside
    // sd_bus_process holding the connection's bus mutex, and an openDAQ status update fires a core
    // event synchronously; doing that here would let a listener that reads a characteristic
    // deadlock against this very thread.
    if (callback)
        callback();
}

void ReconnectingConnection::Shared::publish(LinkState state, const std::string& message)
{
    LinkStateCallback callback;
    {
        std::scoped_lock lock(sync);
        callback = linkStateCallback;
    }

    // Invoked outside the lock: the device sets openDAQ properties and statuses from here, which
    // notifies listeners, and a listener calling back in would deadlock on a non-recursive mutex.
    if (!callback)
        return;

    try
    {
        callback(state, message);
    }
    catch (const std::exception&)
    {
        // The device may be tearing down; a failed status update is not actionable here.
    }
}

void ReconnectingConnection::Shared::supervise()
{
    while (true)
    {
        {
            std::unique_lock lock(sync);
            wake.wait(lock, [this] { return stopping || linkDown; });
            if (stopping)
                return;
        }

        // Destroy the dead connection here rather than in the callback that reported it: a BlueZ
        // connection joins its own D-Bus dispatch thread on the way out, and that dispatch thread
        // is exactly where the disconnect notification came from.
        {
            std::shared_ptr<ble::IConnection> dead;
            {
                std::scoped_lock lock(sync);
                dead = std::move(inner);
                inner.reset();
            }
            dead.reset();
        }

        if (!policy.autoReconnect)
        {
            publish(LinkState::Unrecoverable,
                    "The link to " + identifier + " dropped and AutoReconnect is off.");

            std::unique_lock lock(sync);
            wake.wait(lock, [this] { return stopping; });
            return;
        }

        publish(LinkState::Reconnecting, "The link to " + identifier + " dropped; reconnecting.");

        while (true)
        {
            std::string error;
            std::string note;
            if (reestablish(error, note))
            {
                publish(LinkState::Connected, note);
                break;
            }

            // The reason is carried in the message rather than an attempt counter:
            // updateConnectionStatusWithMessage ignores an unchanged value and message, so an
            // unchanging failure stays quiet while a changed one still reaches listeners.
            publish(LinkState::Reconnecting, error);

            std::unique_lock lock(sync);
            if (stopping)
                return;

            // Waiting on the condition variable rather than sleeping, so shutdown() interrupts a
            // long retry period instantly instead of after it elapses.
            wake.wait_for(lock, policy.retryPeriod, [this] { return stopping; });
            if (stopping)
                return;
        }
    }
}

bool ReconnectingConnection::Shared::reestablish(std::string& error, std::string& note)
{
    {
        std::scoped_lock lock(sync);
        if (stopping)
            return false;
    }

    // BlueZ evicts a non-bonded peripheral from its object tree once it stops being seen, and
    // connect() then fails with "is not known to BlueZ" for as long as nothing is scanning --
    // which would make retrying forever a guarantee of never succeeding. Starting a scan is
    // idempotent in every backend, and the reconnector deliberately never stops one: the scan is a
    // process-wide resource ScanCache owns.
    try
    {
        central->startScan();
    }
    catch (const std::exception&)
    {
        // An adapter that cannot scan cannot connect either; the attempt below reports the reason.
    }

    std::unique_ptr<ble::IConnection> fresh;
    try
    {
        fresh = central->connect(identifier, policy.connectTimeout);
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }

    if (!fresh)
    {
        error = "the backend returned no connection";
        return false;
    }

    const auto remap = buildRemap(frozenServices, fresh->services());

    std::map<std::string, ble::NotificationCallback> wanted;
    {
        std::scoped_lock lock(sync);
        wanted = subscriptions;
    }

    std::vector<std::string> missing;

    // Everything is wired up before the connection is published. A link whose subscriptions
    // silently failed looks connected and never notifies, which is worse than an honest outage --
    // so a failure here throws the connection away and the supervisor tries again.
    for (const auto& [handle, callback] : wanted)
    {
        const auto it = remap.find(handle);
        if (it == remap.end())
        {
            // Structural: the peripheral no longer has this characteristic, so no number of
            // retries brings it back. Take the link anyway -- the rest of the device works -- and
            // let the caller see why in the state message.
            missing.push_back(handle);
            continue;
        }

        try
        {
            fresh->subscribe(it->second, callback);
        }
        catch (const std::exception& e)
        {
            // Operational, and possibly transient: retry rather than report a healthy-looking link
            // that will never deliver a sample.
            error = e.what();
            return false;
        }
    }

    auto self = shared_from_this();
    fresh->setDisconnectCallback([self] { self->onInnerDisconnect(); });

    {
        std::scoped_lock lock(sync);
        if (stopping)
            return false;

        mtuSize = fresh->mtu();
        handleRemap = remap;
        inner = std::move(fresh);
        linkDown = false;

        for (const auto& handle : missing)
            subscriptions.erase(handle);
    }

    if (!missing.empty())
        note = "Reconnected, but " + std::to_string(missing.size()) +
               " subscribed characteristic(s) are no longer exposed by the peripheral.";

    return true;
}

}

#pragma once

#include <bluetooth_module/ble/ble_central.h>
#include <bluetooth_module/common.h>
#include <bluetooth_module/gatt_value.h>
#include <bluetooth_module/reconnecting_connection.h>

#include <opendaq/device_impl.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

BEGIN_NAMESPACE_OPENDAQ_BLUETOOTH

/*!
 * A single connected BLE peripheral, presented as an openDAQ device.
 *
 * The peripheral's GATT database is the device's property tree: one object property per service,
 * and inside it one property per characteristic whose value is the characteristic's value. A
 * characteristic is addressed by name, like anything else in openDAQ --
 * getPropertyValue("Battery Service.Battery Level") -- and is read-only unless GATT reports a
 * write bit, in which case setting it performs the write.
 *
 * Values are served from what was last read. Reading a property additionally schedules a refresh
 * on a background thread, because a property read holds openDAQ's config lock and nothing holding
 * that lock may block on the radio.
 */
class BluetoothDevice final : public Device
{
public:
    BluetoothDevice(const ContextPtr& ctx,
                    const ComponentPtr& parent,
                    const StringPtr& localId,
                    const std::string& identifier,
                    const std::string& displayName,
                    std::shared_ptr<::opendaq_bt::ReconnectingConnection> connection);

    ~BluetoothDevice() override;

    DeviceInfoPtr onGetInfo() override;

    static DeviceInfoPtr createDeviceInfo(const std::string& identifier, const std::string& name);

protected:
    /*!
     * Stops the reconnector when the device leaves the openDAQ tree.
     *
     * removedNoLock, not removed: ComponentImpl::remove() calls removed() while holding the
     * recursive config lock, and the reconnector's supervisor thread takes that same lock through
     * setProtectedPropertyValue. Joining it from there would deadlock.
     */
    void removedNoLock() override;

private:
    void initProperties();

    /*! Adds the ConnectionStatus / ConfigurationStatus statuses, if the context knows the type. */
    void initConnectionStatus();

    /*!
     * Publishes a link state change onto both status containers.
     *
     * Fires a ConnectionStatusChanged core event synchronously on the calling thread, so it must
     * never run on a backend thread: those hold the connection's bus mutex.
     */
    void publishConnectionStatus(const char* value, const std::string& message);

    /*! Reacts to the link going down, coming back, or being given up on. */
    void onLinkStateChanged(::opendaq_bt::ReconnectingConnection::LinkState state,
                            const std::string& message);

    /*!
     * Builds the property tree: one object property per service, one property per characteristic.
     *
     * Handlers are attached afterwards, by attachGattHandlers, so seeding the initial values does
     * not fire the write handlers and push them straight back to the peripheral.
     */
    void initGattProperties();

    /*! Reads every readable characteristic once, so the first browse shows real values. */
    void readInitialValues();

    /*!
     * Attaches the per-characteristic read and write handlers.
     *
     * Must run before any client can connect: openDAQ serialises a HasOnReadListeners flag once,
     * and a mirrored client that missed it never round-trips and caches the value forever.
     */
    void attachGattHandlers();

    /*!
     * Creates a channel for each connected service that a GATT profile understands, so a
     * peripheral implementing a known service produces measurements rather than only being
     * browsable. Services matching no profile are left to their property subtree alone.
     */
    void buildProfileChannels();

    /*!
     * Queues a characteristic to be re-read. Cheap and non-blocking; safe to call under any lock.
     *
     * Skips a characteristic read within the last RefreshHoldOff unless @p force, so browsing the
     * device does not turn into one radio round trip per property per browse.
     */
    void scheduleRefresh(const std::string& handle, bool force = false);

    /*! Queues every readable characteristic, e.g. after the link came back. */
    void scheduleRefreshOfEverything();

    /*! One re-read, on the worker. Called only while a strong reference to the device is held. */
    void refreshOne(const std::string& handle);

    /*!
     * Everything the refresh worker touches, behind a shared_ptr the thread captures.
     *
     * A job upgrades the weak reference below, so releasing it can be what destroys the device --
     * on the worker's own thread. Without the indirection the loop would return into freed
     * members. Same pattern, same reason, as ReconnectingConnection::Shared.
     */
    struct RefreshState
    {
        std::mutex sync;
        std::condition_variable wake;
        std::set<std::string> pending;
        std::map<std::string, std::chrono::steady_clock::time_point> lastRefreshed;
        bool stopping = false;

        /*! Upgraded before the device is touched: openDAQ frees the weak-ref bookkeeping at
         *  strong-count zero, before the destructor that stops this worker ever runs. */
        WeakRefPtr<IDevice> device;
        BluetoothDevice* owner = nullptr;
    };

    /*! The background reader. Never holds a lock across a radio round trip. */
    static void refreshLoop(std::shared_ptr<RefreshState> state);

    /*! Stops and joins the refresh worker. Idempotent. */
    void stopRefreshWorker();

    /*!
     * Writes a value the device itself produced, without the write handler mistaking it for a
     * caller's write.
     *
     * setProtectedPropertyValue fires write handlers, so refreshing a characteristic that is both
     * readable and writable would otherwise push the value it just read straight back to the
     * peripheral, for ever.
     */
    void applyValue(const StringPtr& path, const BaseObjectPtr& value);

    /*! One characteristic, as the device needs to know it. */
    struct CharacteristicBinding
    {
        StringPtr path;        //!< "Battery Service.Battery Level", for logs and diagnostics
        PropertyObjectPtr owner;  //!< the service object the property lives on
        StringPtr name;        //!< the leaf name within that service
        std::string handle;    //!< frozen backend handle, as ReconnectingConnection knows it
        GattValueFormat format = GattValueFormat::Hex;
        bool readable = false;
        bool writable = false;
        bool writeWithResponse = false;
    };

    const std::string identifier;
    const std::string displayName;

    /*!
     * The service objects, kept because openDAQ stores the object handed to ObjectProperty as the
     * live value and freezes only a clone as the property's default. These are the ones to mutate.
     */
    std::vector<PropertyObjectPtr> serviceObjects;

    /*! Every characteristic, keyed by frozen handle. Written once during construction. */
    std::map<std::string, CharacteristicBinding> bindings;

    /*!
     * Shared, not owned outright: profile channels live in the IO folder and use the connection
     * too. A derived destructor body runs before its base's, so this object would otherwise
     * release the connection before GenericDevice tore down the folder holding those channels,
     * and their destructors would unsubscribe through a dangling pointer.
     */
    std::shared_ptr<::opendaq_bt::ReconnectingConnection> connection;

    /*! Its `sync` is a leaf lock: never held while taking another, so a handler may take it. */
    const std::shared_ptr<RefreshState> refreshState;
    std::thread refreshWorker;

    /*! The thread currently applying a value the device produced; see applyValue. */
    std::atomic<std::thread::id> suppressWriteHandlerFor;


    /*! False when the context had no ConnectionStatusType, so updates are skipped rather than throw. */
    bool connectionStatusRegistered = false;
};

END_NAMESPACE_OPENDAQ_BLUETOOTH

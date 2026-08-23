#pragma once

#include <bluetooth_module/ble/ble_central.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace opendaq_bt
{

/*!
 * A BLE connection that survives the link dropping.
 *
 * It is itself an ble::IConnection wrapping the backend's, so everything holding one -- the device
 * and every profile channel in its IO folder -- keeps the same object across an outage. That is
 * the whole point: rebuilding the channels instead would destroy their openDAQ signals, and every
 * StreamReader a consumer had attached with them. Here a reader running through a dropout simply
 * sees a gap in the timestamps and then data again.
 *
 * Lives above the ble_central.h seam and uses no openDAQ types, like ScanCache: link state is
 * reported through a plain callback and the device does the openDAQ work.
 */
class ReconnectingConnection final : public ble::IConnection
{
public:
    enum class LinkState
    {
        Connected,
        Reconnecting,
        Unrecoverable
    };

    /*!
     * Invoked on the supervisor thread whenever the link state changes, never under the internal
     * lock. @p message carries the last failure reason where there is one.
     */
    using LinkStateCallback = std::function<void(LinkState state, const std::string& message)>;

    struct Policy
    {
        /*! Budget for one connection attempt, handed to ICentral::connect. */
        std::chrono::milliseconds connectTimeout{15000};

        /*! Gap between attempts. A failed attempt already costs up to connectTimeout. */
        std::chrono::milliseconds retryPeriod{5000};

        /*! False reports Unrecoverable on the first drop and never retries. */
        bool autoReconnect = true;
    };

    /*! Takes over @p initial, which must be an open connection to @p identifier. */
    ReconnectingConnection(std::shared_ptr<ble::ICentral> central,
                           std::string identifier,
                           std::unique_ptr<ble::IConnection> initial,
                           Policy policy);

    ~ReconnectingConnection() override;

    ReconnectingConnection(const ReconnectingConnection&) = delete;
    ReconnectingConnection& operator=(const ReconnectingConnection&) = delete;

    bool isConnected() const override;

    /*!
     * The GATT database as the *first* connection resolved it, and never replaced.
     *
     * The interface hands back a reference, and BluetoothDevice::resolveCharacteristic returns a
     * reference into it, so swapping the vector on reconnect would dangle. Freezing it also makes
     * the handles published in the GattDatabase property stable for the device's lifetime; a
     * reconnect that renumbers them is absorbed by the remap below instead of being visible.
     */
    const std::vector<ble::Service>& services() const override;

    uint16_t mtu() const override;

    std::vector<uint8_t> read(const std::string& handle) override;

    void write(const std::string& handle,
               const std::vector<uint8_t>& data,
               bool withResponse) override;

    /*!
     * Subscribes, and remembers the subscription so it is re-applied after every reconnect.
     *
     * Called while the link is down it records the callback and returns, so a channel built during
     * an outage is wired up when the peripheral comes back.
     */
    void subscribe(const std::string& handle, ble::NotificationCallback callback) override;

    void unsubscribe(const std::string& handle) override;

    /*! Fired on every drop, reconnected or not. Most callers want setLinkStateCallback instead. */
    void setDisconnectCallback(std::function<void()> callback) override;

    void setLinkStateCallback(LinkStateCallback callback);

    /*!
     * Stops supervising and guarantees no further link state callback. Idempotent.
     *
     * The backend connection is deliberately left open: channels still hold this object after the
     * device's destructor body has run, and their destructors unsubscribe through it.
     */
    void shutdown();

private:
    /*!
     * Everything the supervisor thread touches, held behind a shared_ptr.
     *
     * The thread captures its own reference, so the state outlives this object. That matters
     * because a link state handler holds a reference to the openDAQ device while it runs, and
     * releasing it can be what destroys the device -- and with it this connection -- on the
     * supervisor's own thread. Without the indirection the supervisor would return into freed
     * members. It is the same shared_ptr-state pattern the WinRT and CoreBluetooth backends use
     * for their event handlers, and for the same reason.
     */
    struct Shared;

    const std::shared_ptr<Shared> shared;

    std::thread supervisor;
};

}

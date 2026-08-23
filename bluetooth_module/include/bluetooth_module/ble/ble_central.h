#pragma once

#include <bluetooth_module/ble/ble_types.h>

#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace opendaq_bt::ble
{

/*! Thrown by backends for any BLE-level failure: no adapter, connect timeout, GATT error. */
class BleError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/*!
 * Invoked when a subscribed characteristic notifies, carrying the new value.
 *
 * Called on a backend thread, so implementations of it must be thread-safe and must not block:
 * on Linux this runs on the D-Bus dispatch thread, and stalling it delays every other event on
 * that connection.
 */
using NotificationCallback = std::function<void(const std::vector<uint8_t>& value)>;

/*!
 * An open connection to a single BLE peripheral. Destroying the object disconnects.
 *
 * Implementations must be safe to call from a thread other than the one that created them,
 * because openDAQ invokes property handlers from arbitrary threads.
 */
class IConnection
{
public:
    virtual ~IConnection() = default;

    virtual bool isConnected() const = 0;

    /*! The peripheral's resolved GATT database. Populated before the connection is handed out. */
    virtual const std::vector<Service>& services() const = 0;

    /*! Negotiated ATT MTU in bytes, or 0 if the backend cannot report it. */
    virtual uint16_t mtu() const = 0;

    virtual std::vector<uint8_t> read(const std::string& handle) = 0;

    virtual void write(const std::string& handle,
                       const std::vector<uint8_t>& data,
                       bool withResponse) = 0;

    /*!
     * Starts delivering notifications (or indications) for a characteristic.
     *
     * Only meaningful for characteristics whose properties include CharPropNotify or
     * CharPropIndicate; anything else throws BleError. Subscribing twice to the same handle
     * replaces the callback rather than stacking a second subscription.
     */
    virtual void subscribe(const std::string& handle, NotificationCallback callback) = 0;

    virtual void unsubscribe(const std::string& handle) = 0;

    /*! Invoked on an unsolicited disconnect. Called from a backend thread. */
    virtual void setDisconnectCallback(std::function<void()> callback) = 0;
};

/*!
 * The BLE central role: scanning for peripherals and connecting to one.
 *
 * This is the single seam between the openDAQ-facing code and the per-platform Bluetooth stacks.
 * Nothing above this interface includes an OS Bluetooth header, and nothing below it includes an
 * openDAQ header. Tests substitute a fake implementation, so the whole openDAQ layer is
 * exercisable without a radio.
 */
class ICentral
{
public:
    virtual ~ICentral() = default;

    /*!
     * Whether a usable, powered adapter is present and this process is permitted to use it.
     * On failure @p reason receives a human-readable explanation suitable for surfacing to the
     * user -- missing adapter, adapter powered off, D-Bus permission denied, or the macOS
     * Bluetooth privacy prompt having been declined.
     */
    virtual bool isAvailable(std::string& reason) = 0;

    virtual void startScan() = 0;
    virtual void stopScan() = 0;
    virtual bool isScanning() const = 0;

    /*!
     * Service UUIDs the caller has a use for, in any spelling ble::expandUuid accepts.
     *
     * Only backends that cannot enumerate connected peripherals without being told what to look
     * for need this -- CoreBluetooth's retrieveConnectedPeripheralsWithServices: takes a non-empty
     * service array and has no "everything" form. The others ignore it.
     */
    virtual void setServicesOfInterest(const std::vector<std::string>& /*uuids*/) {}

    /*!
     * Snapshot of everything seen since the scan began. Returns immediately.
     *
     * Also includes every peripheral currently connected to this host, whether or not this scan
     * heard it advertise: a connected peripheral stops advertising, and it is the one kind of
     * peripheral the caller is certain to be able to reach. Those entries carry
     * Advertisement::connected, an rssi of 0 and a lastSeen stamped at call time.
     */
    virtual std::vector<Advertisement> getDiscovered() = 0;

    /*! Connects and resolves the GATT database, or throws BleError. */
    virtual std::unique_ptr<IConnection> connect(const std::string& id,
                                                 std::chrono::milliseconds timeout) = 0;
};

/*! Constructs the backend for the platform this binary was compiled for. */
std::unique_ptr<ICentral> createPlatformCentral();

}

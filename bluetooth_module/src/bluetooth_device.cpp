#include <bluetooth_module/bluetooth_device.h>
#include <bluetooth_module/connection_string.h>
#include <bluetooth_module/gatt_names.h>
#include <bluetooth_module/gatt_value.h>
#include <bluetooth_module/profiles/gatt_profile.h>

#include <coreobjects/property_object_factory.h>
#include <coreobjects/property_object_protected_ptr.h>
#include <coretypes/enumeration_factory.h>
#include <opendaq/component_status_container_private.h>
#include <opendaq/custom_log.h>

#include <cctype>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

BEGIN_NAMESPACE_OPENDAQ_BLUETOOTH

namespace ble = ::opendaq_bt::ble;

namespace
{

/*!
 * How long a characteristic is left alone after being read from the peripheral.
 *
 * Browsing the device now reads every property, and a client mirroring it over the native protocol
 * reads every property too -- so without this, one browse of a peripheral with nine readable
 * characteristics is nine radio round trips, every time. A second is a sane cadence for BLE and
 * bounds a browse storm to one read per characteristic.
 */
constexpr std::chrono::milliseconds RefreshHoldOff{1000};

}

BluetoothDevice::BluetoothDevice(const ContextPtr& ctx,
                                 const ComponentPtr& parent,
                                 const StringPtr& localId,
                                 const std::string& identifier,
                                 const std::string& displayName,
                                 std::shared_ptr<::opendaq_bt::ReconnectingConnection> connection)
    : Device(ctx, parent, localId, nullptr, String(displayName))
    , identifier(identifier)
    , displayName(displayName)
    , connection(std::move(connection))
    , refreshState(std::make_shared<RefreshState>())
{
    refreshState->device = this->template borrowPtr<DevicePtr>();
    refreshState->owner = this;

    initProperties();

    // Order matters. The tree is built empty, then seeded, then given its handlers: seeding
    // through the protected setter fires write handlers, so attaching them first would push every
    // initial value straight back to the peripheral.
    initGattProperties();
    readInitialValues();
    attachGattHandlers();

    initConnectionStatus();
    buildProfileChannels();

    refreshWorker = std::thread([state = refreshState] { refreshLoop(state); });

    // Nothing is hooked to the plain disconnect callback on purpose. It is delivered on the
    // backend's own thread -- the D-Bus dispatch thread on Linux, which holds the connection's bus
    // mutex while it runs -- and every openDAQ property set takes the device's config lock. A
    // caller writing a characteristic holds that lock and then blocks on the bus mutex, so a
    // backend thread taking them the other way round is a deadlock. The link state callback runs
    // on the reconnector's own thread instead, which holds no bus lock.

    // A weak reference, upgraded for the duration of each callback.
    //
    // The reconnector calls this from its own thread, and openDAQ frees a component's weak-ref
    // bookkeeping the moment its strong count reaches zero -- before the destructor body that
    // stops the reconnector ever runs. Capturing a raw `this` therefore crashes whenever the
    // application drops its last reference mid-callback. Upgrading a weak reference closes that:
    // it either fails, and the callback does nothing, or it succeeds and the device cannot be
    // destroyed until the callback returns. It must stay weak -- a strong capture would make the
    // device own itself, which is what LeavesNoDeviceBehind guards against.
    const WeakRefPtr<IDevice> weakSelf = this->template borrowPtr<DevicePtr>();

    this->connection->setLinkStateCallback(
        [this, weakSelf](::opendaq_bt::ReconnectingConnection::LinkState state,
                         const std::string& message) {
            const auto ref = weakSelf.getRef();
            if (!ref.assigned())
                return;

            onLinkStateChanged(state, message);
        });

    // The callback is installed last, so a link that dropped while the channels were being built
    // would have gone unnoticed. Reconcile once rather than trusting the ordering.
    if (!this->connection->isConnected())
        onLinkStateChanged(::opendaq_bt::ReconnectingConnection::LinkState::Reconnecting,
                           "The link dropped while the device was being built.");
}

BluetoothDevice::~BluetoothDevice()
{
    // The reconnector's supervisor and the refresh worker both reach back into this device, so
    // both are stopped before anything else is touched.
    if (connection)
        connection->shutdown();

    stopRefreshWorker();

    if (connection)
    {
        // Drop the callbacks before releasing the connection so a late notification cannot
        // re-enter a half-destroyed device.
        connection->setLinkStateCallback(nullptr);
        connection->setDisconnectCallback(nullptr);

        // Only this object's reference: the profile channels still hold theirs, and the link stays
        // open until GenericDevice has torn down the IO folder holding them.
        connection.reset();
    }
}

void BluetoothDevice::removedNoLock()
{
    if (connection)
        connection->shutdown();

    stopRefreshWorker();

    Device::removedNoLock();
}

DeviceInfoPtr BluetoothDevice::createDeviceInfo(const std::string& identifier,
                                                const std::string& name)
{
    auto info = DeviceInfo(::opendaq_bt::formatConnectionString(identifier), name);
    info.setSerialNumber(identifier);
    info.setConnectionString(::opendaq_bt::formatConnectionString(identifier));

    return info;
}

DeviceInfoPtr BluetoothDevice::onGetInfo()
{
    return createDeviceInfo(identifier, displayName);
}

void BluetoothDevice::initConnectionStatus()
{
    const auto typeManager = this->context.assigned() ? this->context.getTypeManager() : TypeManagerPtr();

    // ContextImpl registers ConnectionStatusType unconditionally, but a device can be built over a
    // context that was assembled by hand, and a missing type must cost a log line rather than the
    // whole device.
    if (!typeManager.assigned() || !typeManager.hasType("ConnectionStatusType"))
    {
        LOG_W("The context has no ConnectionStatusType, so {} publishes no connection status", identifier)
        return;
    }

    try
    {
        const auto connected = Enumeration("ConnectionStatusType", "Connected", typeManager);

        // Both containers, as openDAQ's own native streaming device does: the component status is
        // what a generic component view shows, and the connection status is what
        // Device::getConnectionStatusContainer() exposes and what fires the ConnectionStatusChanged
        // core event that mirroring clients consume.
        this->statusContainer.asPtr<IComponentStatusContainerPrivate>().addStatus(
            String("ConnectionStatus"), connected);

        this->connectionStatusContainer.addConfigurationConnectionStatus(
            String(::opendaq_bt::formatConnectionString(identifier)), connected);

        connectionStatusRegistered = true;
    }
    catch (const std::exception& e)
    {
        LOG_W("Cannot publish the connection status of {}: {}", identifier, e.what())
    }
}

void BluetoothDevice::publishConnectionStatus(const char* value, const std::string& message)
{
    if (!connectionStatusRegistered)
        return;

    try
    {
        const auto status = Enumeration("ConnectionStatusType", value, this->context.getTypeManager());

        this->statusContainer.asPtr<IComponentStatusContainerPrivate>().setStatusWithMessage(
            String("ConnectionStatus"), status, String(message));

        // Returns OPENDAQ_IGNORED when neither the value nor the message changed, so an unchanging
        // failure reason repeated on every retry does not fire a core event every time.
        this->connectionStatusContainer.updateConnectionStatusWithMessage(
            String(::opendaq_bt::formatConnectionString(identifier)),
            status,
            nullptr,
            String(message));
    }
    catch (const std::exception& e)
    {
        // Runs on the reconnector's supervisor thread, where an escaping exception is
        // std::terminate.
        LOG_D("Cannot update the connection status of {}: {}", identifier, e.what())
    }
}

void BluetoothDevice::onLinkStateChanged(::opendaq_bt::ReconnectingConnection::LinkState state,
                                         const std::string& message)
{
    using LinkState = ::opendaq_bt::ReconnectingConnection::LinkState;

    const bool up = state == LinkState::Connected;

    try
    {
        const auto objPtr = this->template borrowPtr<PropertyObjectProtectedPtr>();

        objPtr.setProtectedPropertyValue(String("Connected"), BooleanPtr(up));

        if (up)
        {
            // The ATT MTU is renegotiated per link, so the old figure is not the new one.
            objPtr.setProtectedPropertyValue(String("MtuSize"), Integer(connection->mtu()));
        }
    }
    catch (const std::exception&)
    {
        // The device may already be tearing down; a failed property update is not actionable.
    }

    switch (state)
    {
        case LinkState::Connected:
            LOG_I("Reconnected to {}", identifier)
            publishConnectionStatus("Connected", message);
            break;
        case LinkState::Reconnecting:
            LOG_W("Lost the link to {}: {}", identifier, message)
            publishConnectionStatus("Reconnecting", message);
            break;
        case LinkState::Unrecoverable:
            LOG_W("Giving up on {}: {}", identifier, message)
            publishConnectionStatus("Unrecoverable", message);
            break;
    }

    if (!up)
        return;

    // Queued rather than read here: this runs on the reconnector's supervisor thread, and a round
    // trip per readable characteristic would hold up the next reconnect for as long as the radio
    // takes. The values arrive shortly afterwards, each firing its own change event.
    scheduleRefreshOfEverything();
}

void BluetoothDevice::initProperties()
{
    const auto objPtr = this->template borrowPtr<PropertyObjectPtr>();

    // These mirror the state of the peripheral, so they are read-only to applications. The device
    // still updates them through the protected setter as the connection changes.
    objPtr.addProperty(StringPropertyBuilder("Address", identifier).setReadOnly(True).build());
    objPtr.addProperty(StringPropertyBuilder("Name", displayName).setReadOnly(True).build());
    objPtr.addProperty(
        BoolPropertyBuilder("Connected", connection->isConnected()).setReadOnly(True).build());
    objPtr.addProperty(IntPropertyBuilder("MtuSize", connection->mtu()).setReadOnly(True).build());
}

void BluetoothDevice::initGattProperties()
{
    const auto objPtr = this->template borrowPtr<PropertyObjectPtr>();

    // Seeded with the names the device already uses, so a service called "Name" -- which nothing
    // in the assigned-numbers table produces today, but a future entry could -- is suffixed rather
    // than rejected as a duplicate.
    std::set<std::string> serviceNames = {"Address", "Name", "Connected", "MtuSize"};

    for (const auto& service : connection->services())
    {
        const auto serviceName = propertyNameFor(service.uuid, serviceNames);

        auto serviceObj = PropertyObject();
        std::set<std::string> characteristicNames;

        for (const auto& characteristic : service.characteristics)
        {
            const auto name = propertyNameFor(characteristic.uuid, characteristicNames);
            const auto format = gattValueFormat(characteristic.uuid);

            const bool readable = (characteristic.properties & ble::CharPropRead) != 0;
            const bool withResponse = (characteristic.properties & ble::CharPropWrite) != 0;
            const bool writable =
                withResponse || (characteristic.properties & ble::CharPropWriteWithoutResponse) != 0;

            // The UUID and the flags used to be struct fields. They live in the description now:
            // it is the one piece of metadata openDAQ carries on every property, object properties
            // included. The backend handle is deliberately not published -- it stopped being an
            // addressing key when names became one, and it was always opaque.
            const auto description =
                ble::expandUuid(characteristic.uuid) + " | " +
                ble::characteristicPropertiesToString(characteristic.properties);

            auto builder = PropertyBuilder(String(name))
                               .setValueType(gattCoreType(format))
                               .setDefaultValue(gattDefaultValue(format))
                               .setDescription(String(description))
                               .setReadOnly(BooleanPtr(!writable));

            if (const auto unit = gattUnit(format); unit.assigned())
                builder = builder.setUnit(unit);

            serviceObj.addProperty(builder.build());

            CharacteristicBinding binding;
            binding.path = String(serviceName + "." + name);
            binding.owner = serviceObj;
            binding.name = String(name);
            binding.handle = characteristic.handle;
            binding.format = format;
            binding.readable = readable;
            binding.writable = writable;
            binding.writeWithResponse = withResponse;

            bindings[characteristic.handle] = std::move(binding);
        }

        // Read-only stops the whole service being replaced wholesale; it does not make the
        // characteristics inside read-only, because openDAQ resolves a child path before it checks
        // the parent's flag. The object is kept as well as added: openDAQ stores the object handed
        // to ObjectProperty as the live value and freezes only a clone as the default.
        objPtr.addProperty(ObjectPropertyBuilder(String(serviceName), serviceObj)
                               .setDescription(String(ble::expandUuid(service.uuid)))
                               .setReadOnly(True)
                               .build());

        serviceObjects.push_back(serviceObj);
    }
}

void BluetoothDevice::readInitialValues()
{
    for (const auto& [handle, binding] : bindings)
    {
        if (!binding.readable)
            continue;

        try
        {
            applyValue(binding.path, decodeGattValue(binding.format, connection->read(handle)));
        }
        catch (const std::exception& e)
        {
            // Characteristics commonly require pairing or a prior write; that must not abort the
            // whole device, so the property keeps its default and the reason goes to the log.
            LOG_D("Initial read of characteristic {} failed: {}", binding.path, e.what())
        }
    }
}

void BluetoothDevice::attachGattHandlers()
{
    for (auto& [handle, binding] : bindings)
    {
        // Captured as a pointer, deliberately, and never by value.
        //
        // openDAQ stores a handler's closure in a fixed inline buffer and *bitwise* moves that
        // buffer around (delegate.hpp, spec::inplace's move constructor). A std::string short
        // enough for the small-string optimisation keeps a pointer into its own body, so copying
        // the bytes leaves that pointer aimed at the buffer it came from -- and the handler then
        // reads freed memory. `bindings` is a std::map, so the address of a value is stable for
        // the device's lifetime and a bare pointer is safe to carry.
        const auto* entry = &binding;

        if (binding.readable)
        {
            // The handler runs holding openDAQ's config lock, so it must not go near the radio: it
            // queues the re-read and leaves the stored value to be returned unchanged.
            binding.owner.getOnPropertyValueRead(binding.name) +=
                [this, entry](PropertyObjectPtr&, PropertyValueEventArgsPtr&) {
                    scheduleRefresh(entry->handle);
                };
        }

        if (!binding.writable)
            continue;

        binding.owner.getOnPropertyValueWrite(binding.name) +=
            [this, entry](PropertyObjectPtr&, PropertyValueEventArgsPtr& args) {
                // A value this device produced, not one a caller wrote. Without this a refresh of
                // a characteristic that is both readable and writable would push what it just read
                // straight back to the peripheral, for ever.
                if (suppressWriteHandlerFor.load() == std::this_thread::get_id())
                    return;

                const auto bytes = encodeGattValue(entry->format, args.getValue());

                try
                {
                    connection->write(entry->handle, bytes, entry->writeWithResponse);
                }
                catch (const ble::BleError& e)
                {
                    // Thrown out of the handler on purpose: openDAQ abandons the write and leaves
                    // the property alone, so a failed write never looks like a successful one.
                    DAQ_THROW_EXCEPTION(InvalidStateException, e.what());
                }
            };
    }
}

void BluetoothDevice::applyValue(const StringPtr& path, const BaseObjectPtr& value)
{
    suppressWriteHandlerFor.store(std::this_thread::get_id());

    try
    {
        this->template borrowPtr<PropertyObjectProtectedPtr>().setProtectedPropertyValue(path, value);
    }
    catch (const std::exception& e)
    {
        LOG_D("Cannot publish {} of {}: {}", path, identifier, e.what())
    }

    suppressWriteHandlerFor.store(std::thread::id());
}

void BluetoothDevice::scheduleRefresh(const std::string& handle, bool force)
{
    {
        std::scoped_lock lock(refreshState->sync);
        if (refreshState->stopping)
            return;

        if (!force)
        {
            const auto seen = refreshState->lastRefreshed.find(handle);
            if (seen != refreshState->lastRefreshed.end() &&
                std::chrono::steady_clock::now() - seen->second < RefreshHoldOff)
                return;
        }

        refreshState->pending.insert(handle);
    }

    refreshState->wake.notify_all();
}

void BluetoothDevice::scheduleRefreshOfEverything()
{
    {
        std::scoped_lock lock(refreshState->sync);
        if (refreshState->stopping)
            return;

        for (const auto& [handle, binding] : bindings)
        {
            if (binding.readable)
                refreshState->pending.insert(handle);
        }

        // Past the hold-off: the link just came back, so every value is suspect however recently
        // it was read.
        refreshState->lastRefreshed.clear();
    }

    refreshState->wake.notify_all();
}

void BluetoothDevice::refreshOne(const std::string& handle)
{
    const auto it = bindings.find(handle);
    if (it == bindings.end())
        return;

    std::vector<uint8_t> raw;
    try
    {
        // No lock of any kind is held here. A GATT read is a round trip to the peripheral, and
        // holding the device's config lock across it would stall every other property on it.
        raw = connection->read(handle);
    }
    catch (const std::exception& e)
    {
        // Out of range, or a characteristic that refuses to be read. The property keeps the last
        // value it had, which is the most truthful thing available.
        LOG_D("Refresh of {} failed: {}", it->second.path, e.what())
        return;
    }

    {
        std::scoped_lock lock(refreshState->sync);
        refreshState->lastRefreshed[handle] = std::chrono::steady_clock::now();
    }

    applyValue(it->second.path, decodeGattValue(it->second.format, raw));
}

void BluetoothDevice::refreshLoop(std::shared_ptr<RefreshState> state)
{
    while (true)
    {
        std::string handle;

        {
            std::unique_lock lock(state->sync);
            state->wake.wait(lock, [&] { return state->stopping || !state->pending.empty(); });
            if (state->stopping)
                return;

            handle = *state->pending.begin();
            state->pending.erase(state->pending.begin());
        }

        // Scoped, because releasing this reference may be what destroys the device -- and with it
        // everything but the state block this loop holds by value.
        {
            const auto device = state->device.getRef();
            if (!device.assigned())
                return;

            state->owner->refreshOne(handle);
        }
    }
}

void BluetoothDevice::stopRefreshWorker()
{
    {
        std::scoped_lock lock(refreshState->sync);
        if (refreshState->stopping)
            return;

        refreshState->stopping = true;
        refreshState->pending.clear();
    }

    refreshState->wake.notify_all();

    if (!refreshWorker.joinable())
        return;

    // A refresh holds a reference to the device while it runs, so releasing it can be what brings
    // the device here -- on the worker's own thread. Joining then would join this thread to
    // itself; detaching is safe because the loop owns its state through a shared_ptr.
    if (refreshWorker.get_id() == std::this_thread::get_id())
        refreshWorker.detach();
    else
        refreshWorker.join();
}

void BluetoothDevice::buildProfileChannels()
{
    for (const auto& service : connection->services())
    {
        const auto* profile = findGattProfile(service.uuid);
        if (profile == nullptr)
            continue;

        try
        {
            // Mirrors GenericDevice::createAndAddChannel, which cannot be used directly because it
            // is templated on the channel implementation type while the registry hands back a
            // factory: build the channel parented to the IO folder, then attach it.
            const auto channel = profile->createChannel(
                this->context, this->ioFolder, String(profile->channelLocalId), connection, service);

            this->ioFolder.addItem(channel);

            LOG_I("Added the {} channel for {}", profile->name, identifier)
        }
        catch (const std::exception& e)
        {
            // A service can be advertised but unusable -- missing its measurement characteristic,
            // or refusing notifications without pairing. That must not cost the caller the rest of
            // the device, which is still perfectly browsable.
            LOG_W("Cannot add the {} channel for {}: {}", profile->name, identifier, e.what())
        }
    }
}

END_NAMESPACE_OPENDAQ_BLUETOOTH

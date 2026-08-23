#pragma once

#include <bluetooth_module/ble/ble_central.h>
#include <bluetooth_module/common.h>
#include <bluetooth_module/scan_cache.h>

#include <opendaq/module_impl.h>

#include <memory>
#include <mutex>

BEGIN_NAMESPACE_OPENDAQ_BLUETOOTH

/*!
 * openDAQ module exposing nearby Bluetooth Low Energy peripherals as openDAQ devices.
 *
 * Discovery reports the peripherals the host's adapter can currently hear whose advertisement
 * claims a service this module has a profile for, each carrying a "bluetooth://<identifier>"
 * connection string. Passing one of those to Instance::addDevice() connects to the peripheral and
 * returns a BluetoothDevice whose GattDatabase property mirrors the peripheral's GATT database.
 *
 * The profile filter narrows the *list* only, and can be turned off with the FilterByKnownProfiles
 * module option; any peripheral remains connectable by connection string either way.
 */
class BluetoothModule final : public Module
{
public:
    explicit BluetoothModule(ContextPtr context);

    /*!
     * Testing seam: runs the module against a caller-supplied BLE backend instead of the
     * platform one, so the whole openDAQ layer can be exercised without a radio. An unassigned
     * @p config means the defaults; tests generally pass ScanWarmupMs = 0, since a scripted
     * backend has no radio latency to wait out.
     */
    BluetoothModule(ContextPtr context,
                    std::unique_ptr<::opendaq_bt::ble::ICentral> central,
                    PropertyObjectPtr config = nullptr);

    ~BluetoothModule() override;

    ListPtr<IDeviceInfo> onGetAvailableDevices() override;
    DictPtr<IString, IDeviceType> onGetAvailableDeviceTypes() override;
    DevicePtr onCreateDevice(const StringPtr& connectionString,
                             const ComponentPtr& parent,
                             const PropertyObjectPtr& config) override;

    /*! The device type registering the "bluetooth" connection string prefix. */
    static DeviceTypePtr createDeviceType();

    /*! Default configuration attached to the device type; see the README for the keys. */
    static PropertyObjectPtr createDefaultConfig();

private:
    void initScanCache(const PropertyObjectPtr& config);
    void applyModuleOptions();

    // Written once during construction, before the module is published, and only read afterwards,
    // so onGetAvailableDevices needs no synchronisation beyond the lock it already takes.
    bool filterByKnownProfiles = true;

    std::mutex sync;

    /*!
     * Shared, not owned outright: a device's reconnector needs the backend for every retry, and a
     * device can outlive the module that made it. std::shared_ptr converts implicitly from the
     * std::unique_ptr the constructors take, so no caller changes.
     */
    std::shared_ptr<::opendaq_bt::ble::ICentral> central;
    std::unique_ptr<::opendaq_bt::ScanCache> scanCache;
};

END_NAMESPACE_OPENDAQ_BLUETOOTH

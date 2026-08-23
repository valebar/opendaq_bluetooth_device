#include <bluetooth_module/bluetooth_device.h>
#include <bluetooth_module/bluetooth_module.h>
#include <bluetooth_module/connection_string.h>
#include <bluetooth_module/profiles/gatt_profile.h>
#include <bluetooth_module/reconnecting_connection.h>
#include <bluetooth_module/version.h>

#include <coretypes/version_info_factory.h>
#include <opendaq/device_type_factory.h>
#include <opendaq/custom_log.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

BEGIN_NAMESPACE_OPENDAQ_BLUETOOTH

namespace ble = ::opendaq_bt::ble;

namespace
{

constexpr int DefaultScanWarmupMs = 2000;
constexpr int DefaultDeviceTimeoutMs = 30000;
constexpr int DefaultConnectionTimeoutMs = 15000;
constexpr int DefaultReconnectIntervalMs = 5000;
constexpr bool DefaultFilterByKnownProfiles = true;
constexpr bool DefaultAutoReconnect = true;

/*! Reads an integer from a config object that may be unassigned or missing the property. */
int readIntConfig(const PropertyObjectPtr& config, const std::string& name, int fallback)
{
    if (!config.assigned() || !config.hasProperty(name))
        return fallback;

    const auto value = config.getPropertyValue(name);
    if (!value.assigned())
        return fallback;

    return static_cast<int>(static_cast<Int>(value));
}

/*! Reads a boolean from a config object that may be unassigned or missing the property. */
bool readBoolConfig(const PropertyObjectPtr& config, const std::string& name, bool fallback)
{
    if (!config.assigned() || !config.hasProperty(name))
        return fallback;

    const auto value = config.getPropertyValue(name);
    if (!value.assigned())
        return fallback;

    return static_cast<bool>(static_cast<Bool>(value));
}

std::string joinUuids(const std::vector<std::string>& uuids)
{
    std::string joined;
    for (const auto& uuid : uuids)
    {
        if (!joined.empty())
            joined += ", ";
        joined += uuid;
    }
    return joined;
}

DeviceInfoPtr createDeviceInfo(const ble::Advertisement& advertisement)
{
    const auto connectionString = ::opendaq_bt::formatConnectionString(advertisement.id);
    const auto name = advertisement.name.empty() ? ("Bluetooth device " + advertisement.id)
                                                 : advertisement.name;

    auto info = DeviceInfo(connectionString, name);
    info.setDeviceType(BluetoothModule::createDeviceType());
    info.setSerialNumber(advertisement.id);

    // Discovery-time detail an application can use to pick a device without connecting first.
    // These must be added before the object is returned, because openDAQ freezes it afterwards.
    info.addProperty(StringProperty("Address", advertisement.id));
    // Rssi is present whatever the link state, so the DeviceInfo schema does not vary with it,
    // but for a peripheral connected to this host it reads 0 because there is no measurement --
    // not because the signal is strong. Connected is what tells those two apart.
    info.addProperty(IntProperty("Rssi", advertisement.rssi));
    info.addProperty(BoolProperty("Connected", advertisement.connected));
    info.addProperty(StringProperty("AdvertisedServices", joinUuids(advertisement.serviceUuids)));

    if (!advertisement.manufacturerData.empty())
    {
        const auto companyId = advertisement.manufacturerData.begin()->first;
        info.addProperty(IntProperty("ManufacturerCompanyId", companyId));
    }

    return info;
}

}

BluetoothModule::BluetoothModule(ContextPtr context)
    : BluetoothModule(std::move(context), ble::createPlatformCentral())
{
}

BluetoothModule::BluetoothModule(ContextPtr context,
                                 std::unique_ptr<ble::ICentral> central,
                                 PropertyObjectPtr config)
    : Module(ModuleName,
             VersionInfo(BLUETOOTH_MODULE_MAJOR_VERSION,
                         BLUETOOTH_MODULE_MINOR_VERSION,
                         BLUETOOTH_MODULE_PATCH_VERSION),
             std::move(context),
             ModuleName)
    , central(std::move(central))
{
    const auto effectiveConfig = config.assigned() ? config : createDefaultConfig();

    filterByKnownProfiles =
        readBoolConfig(effectiveConfig, "FilterByKnownProfiles", DefaultFilterByKnownProfiles);
    applyModuleOptions();

    initScanCache(effectiveConfig);
}

BluetoothModule::~BluetoothModule()
{
    // Order matters: the cache holds a reference to the backend and stops the scan on teardown.
    // The reset below only drops the module's own reference -- a device still reconnecting keeps
    // the backend alive until it is destroyed too.
    scanCache.reset();
    central.reset();
}

/*!
 * Applies module-level settings from the instance's config providers, overriding whatever the
 * constructor's config object said.
 *
 * Module options take precedence over the constructor defaults. DEFINE_MODULE_EXPORTS gives
 * the ModuleManager only createModule(IModule**, IContext*), so the config-taking constructor is a
 * test-only seam and in production the config is always createDefaultConfig() -- whose
 * FilterByKnownProfiles is indistinguishable from a value a caller set deliberately. If the config
 * won, the production key would be dead on arrival.
 *
 * Note that openDAQ runs no config provider unless the application asks for one, so these options
 * are populated only when the host adds a JSON, environment or command line provider; see README.
 */
void BluetoothModule::applyModuleOptions()
{
    const DictPtr<IString, IBaseObject> options = context.getModuleOptions(moduleInfo.getId());
    if (!options.assigned() || options.getCount() == 0)
        return;

    const BaseObjectPtr filter = options.getOrDefault("FilterByKnownProfiles");
    if (!filter.assigned())
        return;

    if (filter.getCoreType() != CoreType::ctBool)
    {
        // Providers coerce "true"/"false" to a boolean, but anything else arrives as a string and
        // would otherwise be ignored in silence -- which reads exactly like the option not working.
        LOG_W("Ignoring module option FilterByKnownProfiles: expected a boolean, got {}",
              filter.toString())
        return;
    }

    filterByKnownProfiles = static_cast<bool>(static_cast<Bool>(filter));
}

void BluetoothModule::initScanCache(const PropertyObjectPtr& config)
{
    using std::chrono::milliseconds;

    // The profile registry is the only thing that knows which services this module can use, and a
    // backend that can only enumerate connected peripherals by service (CoreBluetooth) has to be
    // told. Every other backend ignores this.
    std::vector<std::string> servicesOfInterest;
    servicesOfInterest.reserve(gattProfiles().size());
    for (const auto& profile : gattProfiles())
        servicesOfInterest.push_back(profile.serviceUuid);

    central->setServicesOfInterest(servicesOfInterest);

    scanCache = std::make_unique<::opendaq_bt::ScanCache>(
        *central,
        milliseconds(readIntConfig(config, "DeviceTimeoutMs", DefaultDeviceTimeoutMs)),
        milliseconds(readIntConfig(config, "ScanWarmupMs", DefaultScanWarmupMs)));
}

PropertyObjectPtr BluetoothModule::createDefaultConfig()
{
    auto config = PropertyObject();

    config.addProperty(IntProperty("ScanWarmupMs", DefaultScanWarmupMs));
    config.addProperty(IntProperty("DeviceTimeoutMs", DefaultDeviceTimeoutMs));
    config.addProperty(IntProperty("ConnectionTimeoutMs", DefaultConnectionTimeoutMs));
    config.addProperty(BoolProperty("AutoReconnect", DefaultAutoReconnect));
    config.addProperty(IntProperty("ReconnectIntervalMs", DefaultReconnectIntervalMs));
    config.addProperty(BoolProperty("FilterByKnownProfiles", DefaultFilterByKnownProfiles));

    return config;
}

DeviceTypePtr BluetoothModule::createDeviceType()
{
    // The fourth argument is the connection string prefix: openDAQ splits an incoming connection
    // string on "://" and routes it to whichever module registered the matching prefix. Passing
    // "bluetooth" here is precisely what makes "bluetooth://..." reach this module.
    return DeviceType(BluetoothDeviceTypeId,
                      "Bluetooth LE device",
                      "A Bluetooth Low Energy peripheral, browsed over GATT",
                      ConnectionStringPrefix,
                      createDefaultConfig());
}

DictPtr<IString, IDeviceType> BluetoothModule::onGetAvailableDeviceTypes()
{
    auto types = Dict<IString, IDeviceType>();

    const auto deviceType = createDeviceType();
    types.set(deviceType.getId(), deviceType);

    return types;
}

ListPtr<IDeviceInfo> BluetoothModule::onGetAvailableDevices()
{
    std::scoped_lock lock(sync);

    auto devices = List<IDeviceInfo>();

    std::string reason;
    if (!central->isAvailable(reason))
    {
        // Not an error: a host with no adapter, or with Bluetooth switched off, legitimately has
        // no Bluetooth devices to offer. Returning empty keeps discovery working for other modules.
        LOG_W("Bluetooth is unavailable, reporting no devices: {}", reason)
        return devices;
    }

    const auto discovered = scanCache->snapshot();

    for (const auto& advertisement : discovered)
    {
        // Strict on purpose: an advertisement with no service UUIDs is hidden too. Plenty of
        // peripherals advertise only a name and manufacturer data and reveal their services only
        // once connected, so this does hide some devices that would in fact work -- they remain
        // reachable by connection string, which the filter does not touch. The alternative,
        // treating an empty list as "unknown, so show it", puts most of the room back on the list
        // and defeats the point.
        if (filterByKnownProfiles && !advertisesKnownGattProfile(advertisement))
            continue;

        devices.pushBack(createDeviceInfo(advertisement));
    }

    const auto hidden = discovered.size() - static_cast<size_t>(devices.getCount());
    if (hidden > 0)
    {
        // Keep the braces: openDAQ's log macros carry their own trailing semicolon, so a body
        // without braces would swallow the return below.
        LOG_D("Hid {} peripheral(s) advertising no service this module has a profile for", hidden)
    }

    return devices;
}

DevicePtr BluetoothModule::onCreateDevice(const StringPtr& connectionString,
                                          const ComponentPtr& parent,
                                          const PropertyObjectPtr& config)
{
    std::string identifier;
    std::string error;

    if (!::opendaq_bt::tryParseConnectionString(connectionString.toStdString(), identifier, error))
        DAQ_THROW_EXCEPTION(InvalidParameterException, error.c_str());

    std::string reason;
    if (!central->isAvailable(reason))
        DAQ_THROW_EXCEPTION(NotFoundException, ("Bluetooth is unavailable: " + reason).c_str());

    // ConnectionTimeoutMs, AutoReconnect and ReconnectIntervalMs are the keys live on this path.
    // FilterByKnownProfiles is a discovery-time setting and reading it from a per-device config
    // here would do nothing.
    const auto timeout =
        std::chrono::milliseconds(readIntConfig(config, "ConnectionTimeoutMs", DefaultConnectionTimeoutMs));

    ::opendaq_bt::ReconnectingConnection::Policy policy;
    policy.connectTimeout = timeout;
    policy.retryPeriod = std::chrono::milliseconds(
        std::max(0, readIntConfig(config, "ReconnectIntervalMs", DefaultReconnectIntervalMs)));
    policy.autoReconnect = readBoolConfig(config, "AutoReconnect", DefaultAutoReconnect);

    LOG_I("Connecting to Bluetooth device {}", identifier)

    std::unique_ptr<ble::IConnection> connection;
    try
    {
        connection = central->connect(identifier, timeout);
    }
    catch (const ble::BleError& e)
    {
        // A raw std::exception would reach the caller as a bare error code with openDAQ's generic
        // wrapper text. Rethrowing as an openDAQ exception puts the actual reason -- adapter off,
        // peripheral out of range, pairing required -- into the error info chain where an
        // application can retrieve it.
        LOG_E("Connecting to {} failed: {}", identifier, e.what())
        DAQ_THROW_EXCEPTION(NotFoundException, e.what());
    }

    // Prefer the advertised name for display, falling back to the identifier when the peripheral
    // never advertised one.
    std::string displayName;
    for (const auto& advertisement : scanCache->snapshot())
    {
        if (advertisement.id == identifier && !advertisement.name.empty())
        {
            displayName = advertisement.name;
            break;
        }
    }
    if (displayName.empty())
        displayName = "Bluetooth device " + identifier;

    // Wrapped before the device sees it, so the device and its channels hold one object for their
    // whole lives and a dropped link never invalidates a signal or a consumer's reader.
    auto reconnecting = std::make_shared<::opendaq_bt::ReconnectingConnection>(
        central, identifier, std::move(connection), policy);

    return createWithImplementation<IDevice, BluetoothDevice>(
        context,
        parent,
        String(::opendaq_bt::identifierToLocalId(identifier)),
        identifier,
        displayName,
        std::move(reconnecting));
}

END_NAMESPACE_OPENDAQ_BLUETOOTH

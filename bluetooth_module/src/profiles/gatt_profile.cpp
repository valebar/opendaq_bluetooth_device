#include <bluetooth_module/ble/ble_types.h>
#include <bluetooth_module/profiles/cycling_power_channel.h>
#include <bluetooth_module/profiles/cycling_speed_cadence_channel.h>
#include <bluetooth_module/profiles/environmental_sensing_channel.h>
#include <bluetooth_module/profiles/fitness_machine_channel.h>
#include <bluetooth_module/profiles/gatt_profile.h>
#include <bluetooth_module/profiles/health_thermometer_channel.h>
#include <bluetooth_module/profiles/heart_rate_channel.h>

BEGIN_NAMESPACE_OPENDAQ_BLUETOOTH

const std::vector<GattProfile>& gattProfiles()
{
    static const std::vector<GattProfile> profiles = {
        GattProfile{
            HeartRateChannel::serviceUuid(),
            "Heart Rate",
            "HeartRate",
            [](const ContextPtr& context,
               const ComponentPtr& parent,
               const StringPtr& localId,
               const std::shared_ptr<::opendaq_bt::ble::IConnection>& connection,
               const ::opendaq_bt::ble::Service& service) {
                return createWithImplementation<IChannel, HeartRateChannel>(
                    context, parent, localId, connection, service);
            }},
        GattProfile{
            CyclingPowerChannel::serviceUuid(),
            "Cycling Power",
            "CyclingPower",
            [](const ContextPtr& context,
               const ComponentPtr& parent,
               const StringPtr& localId,
               const std::shared_ptr<::opendaq_bt::ble::IConnection>& connection,
               const ::opendaq_bt::ble::Service& service) {
                return createWithImplementation<IChannel, CyclingPowerChannel>(
                    context, parent, localId, connection, service);
            }},
        GattProfile{
            CyclingSpeedCadenceChannel::serviceUuid(),
            "Cycling Speed and Cadence",
            "CyclingSpeedCadence",
            [](const ContextPtr& context,
               const ComponentPtr& parent,
               const StringPtr& localId,
               const std::shared_ptr<::opendaq_bt::ble::IConnection>& connection,
               const ::opendaq_bt::ble::Service& service) {
                return createWithImplementation<IChannel, CyclingSpeedCadenceChannel>(
                    context, parent, localId, connection, service);
            }},
        GattProfile{
            FitnessMachineChannel::serviceUuid(),
            "Fitness Machine",
            "FitnessMachine",
            [](const ContextPtr& context,
               const ComponentPtr& parent,
               const StringPtr& localId,
               const std::shared_ptr<::opendaq_bt::ble::IConnection>& connection,
               const ::opendaq_bt::ble::Service& service) {
                return createWithImplementation<IChannel, FitnessMachineChannel>(
                    context, parent, localId, connection, service);
            }},
        GattProfile{
            HealthThermometerChannel::serviceUuid(),
            "Health Thermometer",
            "HealthThermometer",
            [](const ContextPtr& context,
               const ComponentPtr& parent,
               const StringPtr& localId,
               const std::shared_ptr<::opendaq_bt::ble::IConnection>& connection,
               const ::opendaq_bt::ble::Service& service) {
                return createWithImplementation<IChannel, HealthThermometerChannel>(
                    context, parent, localId, connection, service);
            }},
        GattProfile{
            EnvironmentalSensingChannel::serviceUuid(),
            "Environmental Sensing",
            "EnvironmentalSensing",
            [](const ContextPtr& context,
               const ComponentPtr& parent,
               const StringPtr& localId,
               const std::shared_ptr<::opendaq_bt::ble::IConnection>& connection,
               const ::opendaq_bt::ble::Service& service) {
                return createWithImplementation<IChannel, EnvironmentalSensingChannel>(
                    context, parent, localId, connection, service);
            }},
    };

    return profiles;
}

const GattProfile* findGattProfile(const std::string& serviceUuid)
{
    // Compare in expanded form so a peripheral advertising the 16-bit shorthand and one giving the
    // full 128-bit UUID resolve to the same profile.
    const auto wanted = ::opendaq_bt::ble::expandUuid(serviceUuid);

    for (const auto& profile : gattProfiles())
    {
        if (::opendaq_bt::ble::expandUuid(profile.serviceUuid) == wanted)
            return &profile;
    }

    return nullptr;
}

bool advertisesKnownGattProfile(const ::opendaq_bt::ble::Advertisement& advertisement)
{
    // Backend asymmetry worth knowing about: on Linux this list is BlueZ's Device1.UUIDs, a
    // persistent cache that unions advertised UUIDs with everything resolved during any earlier
    // connection -- so Linux passes peripherals macOS and Windows will not, and a peripheral
    // connected to once stays visible there. macOS and WinRT merge advertisements with "last
    // non-empty list wins", so a later, sparser advertisement does not un-hide a device.
    for (const auto& serviceUuid : advertisement.serviceUuids)
    {
        // findGattProfile expands both sides, which matters here: a peripheral may advertise the
        // 16-bit shorthand ("180D") and BlueZ copies its UUIDs through without expanding them.
        if (findGattProfile(serviceUuid) != nullptr)
            return true;
    }

    return false;
}

END_NAMESPACE_OPENDAQ_BLUETOOTH

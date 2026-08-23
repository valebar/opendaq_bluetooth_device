#include <bluetooth_module/ble/ble_types.h>
#include <bluetooth_module/gatt_names.h>

#include <cctype>
#include <string>
#include <unordered_map>

namespace opendaq_bt
{

namespace
{

/*!
 * A deliberately small subset of the Bluetooth SIG assigned-numbers registry: the services and
 * characteristics a person browsing a GATT tree is most likely to meet, plus Nordic UART because
 * it is ubiquitous on development hardware. Unknown UUIDs simply render as UUIDs.
 */
const std::unordered_map<std::string, const char*>& nameTable()
{
    static const std::unordered_map<std::string, const char*> table = {
        // Services
        {"00001800-0000-1000-8000-00805f9b34fb", "Generic Access"},
        {"00001801-0000-1000-8000-00805f9b34fb", "Generic Attribute"},
        {"0000180a-0000-1000-8000-00805f9b34fb", "Device Information"},
        {"0000180d-0000-1000-8000-00805f9b34fb", "Heart Rate"},
        {"0000180f-0000-1000-8000-00805f9b34fb", "Battery Service"},
        {"00001810-0000-1000-8000-00805f9b34fb", "Blood Pressure"},
        {"00001816-0000-1000-8000-00805f9b34fb", "Cycling Speed and Cadence"},
        {"00001818-0000-1000-8000-00805f9b34fb", "Cycling Power"},
        {"00001826-0000-1000-8000-00805f9b34fb", "Fitness Machine"},
        {"0000181a-0000-1000-8000-00805f9b34fb", "Environmental Sensing"},
        {"0000181c-0000-1000-8000-00805f9b34fb", "User Data"},
        {"00001812-0000-1000-8000-00805f9b34fb", "Human Interface Device"},
        {"0000fe59-0000-1000-8000-00805f9b34fb", "Nordic DFU"},
        {"6e400001-b5a3-f393-e0a9-e50e24dcca9e", "Nordic UART"},

        // Characteristics
        {"00002a00-0000-1000-8000-00805f9b34fb", "Device Name"},
        {"00002a01-0000-1000-8000-00805f9b34fb", "Appearance"},
        {"00002a04-0000-1000-8000-00805f9b34fb", "Preferred Connection Parameters"},
        {"00002a05-0000-1000-8000-00805f9b34fb", "Service Changed"},
        {"00002a19-0000-1000-8000-00805f9b34fb", "Battery Level"},
        {"00002a23-0000-1000-8000-00805f9b34fb", "System ID"},
        {"00002a24-0000-1000-8000-00805f9b34fb", "Model Number"},
        {"00002a25-0000-1000-8000-00805f9b34fb", "Serial Number"},
        {"00002a26-0000-1000-8000-00805f9b34fb", "Firmware Revision"},
        {"00002a27-0000-1000-8000-00805f9b34fb", "Hardware Revision"},
        {"00002a28-0000-1000-8000-00805f9b34fb", "Software Revision"},
        {"00002a29-0000-1000-8000-00805f9b34fb", "Manufacturer Name"},
        {"00002a37-0000-1000-8000-00805f9b34fb", "Heart Rate Measurement"},
        {"00002a38-0000-1000-8000-00805f9b34fb", "Body Sensor Location"},
        {"00002a39-0000-1000-8000-00805f9b34fb", "Heart Rate Control Point"},
        {"00002a55-0000-1000-8000-00805f9b34fb", "SC Control Point"},
        {"00002a5b-0000-1000-8000-00805f9b34fb", "CSC Measurement"},
        {"00002a5c-0000-1000-8000-00805f9b34fb", "CSC Feature"},
        {"00002a5d-0000-1000-8000-00805f9b34fb", "Sensor Location"},
        {"00002a63-0000-1000-8000-00805f9b34fb", "Cycling Power Measurement"},
        {"00002a64-0000-1000-8000-00805f9b34fb", "Cycling Power Vector"},
        {"00002a65-0000-1000-8000-00805f9b34fb", "Cycling Power Feature"},
        {"00002a66-0000-1000-8000-00805f9b34fb", "Cycling Power Control Point"},
        {"00002a6d-0000-1000-8000-00805f9b34fb", "Pressure"},
        {"00002a6e-0000-1000-8000-00805f9b34fb", "Temperature"},
        {"00002a6f-0000-1000-8000-00805f9b34fb", "Humidity"},
        {"00002aa6-0000-1000-8000-00805f9b34fb", "Central Address Resolution"},
        {"00002acc-0000-1000-8000-00805f9b34fb", "Fitness Machine Feature"},

        // The four non-bike Fitness Machine data characteristics are named but not decoded. A
        // treadmill or rower hosts the same service, and browsing one readably is what tells a
        // user why it produced no channels.
        {"00002acd-0000-1000-8000-00805f9b34fb", "Treadmill Data"},
        {"00002ace-0000-1000-8000-00805f9b34fb", "Cross Trainer Data"},
        {"00002acf-0000-1000-8000-00805f9b34fb", "Step Climber Data"},
        {"00002ad0-0000-1000-8000-00805f9b34fb", "Stair Climber Data"},
        {"00002ad1-0000-1000-8000-00805f9b34fb", "Rower Data"},

        {"00002ad2-0000-1000-8000-00805f9b34fb", "Indoor Bike Data"},
        {"00002ad3-0000-1000-8000-00805f9b34fb", "Training Status"},
        {"00002ad4-0000-1000-8000-00805f9b34fb", "Supported Speed Range"},
        {"00002ad5-0000-1000-8000-00805f9b34fb", "Supported Inclination Range"},
        {"00002ad6-0000-1000-8000-00805f9b34fb", "Supported Resistance Level Range"},
        {"00002ad7-0000-1000-8000-00805f9b34fb", "Supported Heart Rate Range"},
        {"00002ad8-0000-1000-8000-00805f9b34fb", "Supported Power Range"},
        {"00002ad9-0000-1000-8000-00805f9b34fb", "Fitness Machine Control Point"},
        {"00002ada-0000-1000-8000-00805f9b34fb", "Fitness Machine Status"},
        {"6e400002-b5a3-f393-e0a9-e50e24dcca9e", "Nordic UART RX"},
        {"6e400003-b5a3-f393-e0a9-e50e24dcca9e", "Nordic UART TX"},
    };

    return table;
}

}

std::string wellKnownGattName(const std::string& uuid)
{
    const auto expanded = ble::expandUuid(uuid);

    const auto& table = nameTable();
    const auto it = table.find(expanded);

    return it == table.end() ? std::string{} : std::string(it->second);
}

std::string shortUuid(const std::string& uuid)
{
    const auto expanded = ble::expandUuid(uuid);

    if (expanded.size() != 36)
        return expanded;

    std::string result;

    // Members of the SIG base range are universally referred to by their 16-bit slot.
    const bool inSigBaseRange = expanded.compare(8, 28, "-0000-1000-8000-00805f9b34fb") == 0;
    if (inSigBaseRange && expanded.compare(0, 4, "0000") == 0)
        result = expanded.substr(4, 4);
    else
        result = expanded.substr(0, 8);

    for (auto& c : result)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    return result;
}

std::string displayLabel(const std::string& uuid)
{
    const auto known = wellKnownGattName(uuid);
    return known.empty() ? shortUuid(uuid) : known;
}

}

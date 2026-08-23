#include <bluetooth_module/ble/ble_types.h>
#include <bluetooth_module/gatt_names.h>
#include <bluetooth_module/gatt_value.h>
#include <bluetooth_module/profiles/heart_rate_measurement.h>

#include <coreobjects/unit_factory.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>

BEGIN_NAMESPACE_OPENDAQ_BLUETOOTH

namespace ble = ::opendaq_bt::ble;

namespace
{

/*!
 * The characteristics whose layout the SIG pins down to a single scalar, and nothing else.
 *
 * Heart Rate Measurement is deliberately absent even though this module decodes it elsewhere: it
 * is a variable-length structure, it cannot be read at all, and the channel is where it belongs.
 */
const std::unordered_map<std::string, GattValueFormat>& formatTable()
{
    static const std::unordered_map<std::string, GattValueFormat> table = {
        {"00002a00-0000-1000-8000-00805f9b34fb", GattValueFormat::Utf8Text},  // Device Name
        {"00002a24-0000-1000-8000-00805f9b34fb", GattValueFormat::Utf8Text},  // Model Number
        {"00002a25-0000-1000-8000-00805f9b34fb", GattValueFormat::Utf8Text},  // Serial Number
        {"00002a26-0000-1000-8000-00805f9b34fb", GattValueFormat::Utf8Text},  // Firmware Revision
        {"00002a27-0000-1000-8000-00805f9b34fb", GattValueFormat::Utf8Text},  // Hardware Revision
        {"00002a28-0000-1000-8000-00805f9b34fb", GattValueFormat::Utf8Text},  // Software Revision
        {"00002a29-0000-1000-8000-00805f9b34fb", GattValueFormat::Utf8Text},  // Manufacturer Name
        {"00002a19-0000-1000-8000-00805f9b34fb", GattValueFormat::Uint8},     // Battery Level
        {"00002a01-0000-1000-8000-00805f9b34fb", GattValueFormat::Uint16},    // Appearance
        {"00002a38-0000-1000-8000-00805f9b34fb", GattValueFormat::BodySensorLocation},
    };

    return table;
}

/*! Little-endian, which is what every GATT integer is. */
Int readLittleEndian(const std::vector<uint8_t>& raw, size_t count)
{
    Int value = 0;
    for (size_t i = 0; i < count; ++i)
        value |= static_cast<Int>(raw[i]) << (8 * i);
    return value;
}

}

GattValueFormat gattValueFormat(const std::string& uuid)
{
    const auto& table = formatTable();
    const auto it = table.find(ble::expandUuid(uuid));
    return it == table.end() ? GattValueFormat::Hex : it->second;
}

CoreType gattCoreType(GattValueFormat format)
{
    switch (format)
    {
        case GattValueFormat::Uint8:
        case GattValueFormat::Uint16:
            return ctInt;
        case GattValueFormat::Hex:
        case GattValueFormat::Utf8Text:
        case GattValueFormat::BodySensorLocation:
            break;
    }

    return ctString;
}

UnitPtr gattUnit(GattValueFormat format)
{
    // Battery Level is the only scalar here with a unit worth stating; Appearance is an enumerated
    // code, not a measurement.
    if (format == GattValueFormat::Uint8)
        return Unit("%", -1, "percent", "ratio");

    return nullptr;
}

BaseObjectPtr gattDefaultValue(GattValueFormat format)
{
    return gattCoreType(format) == ctInt ? BaseObjectPtr(Integer(0)) : BaseObjectPtr(String(""));
}

BaseObjectPtr decodeGattValue(GattValueFormat format, const std::vector<uint8_t>& raw)
{
    switch (format)
    {
        case GattValueFormat::Utf8Text:
        {
            // Peripherals commonly NUL-pad a fixed-width string field.
            auto text = std::string(raw.begin(), raw.end());
            while (!text.empty() && text.back() == '\0')
                text.pop_back();
            return String(text);
        }

        case GattValueFormat::Uint8:
            if (raw.size() >= 1)
                return Integer(readLittleEndian(raw, 1));
            break;

        case GattValueFormat::Uint16:
            if (raw.size() >= 2)
                return Integer(readLittleEndian(raw, 2));
            break;

        case GattValueFormat::BodySensorLocation:
            if (raw.size() >= 1)
                return String(::opendaq_bt::profiles::bodySensorLocationName(raw[0]));
            break;

        case GattValueFormat::Hex:
            break;
    }

    // Short of what the format promised. Real peripherals do send truncated frames, and losing the
    // bytes would be worse than showing them: fall back to hex rather than inventing a value.
    if (gattCoreType(format) == ctInt)
        return Integer(0);

    return String(toHex(raw));
}

std::vector<uint8_t> encodeGattValue(GattValueFormat format, const BaseObjectPtr& value)
{
    if (!value.assigned())
        DAQ_THROW_EXCEPTION(InvalidParameterException, "A characteristic value must not be null.");

    switch (format)
    {
        case GattValueFormat::Uint8:
        case GattValueFormat::Uint16:
        {
            const auto count = format == GattValueFormat::Uint8 ? 1u : 2u;
            const Int raw = value;

            const Int limit = format == GattValueFormat::Uint8 ? 0xFF : 0xFFFF;
            if (raw < 0 || raw > limit)
                DAQ_THROW_EXCEPTION(
                    InvalidParameterException,
                    ("Value " + std::to_string(raw) + " does not fit this characteristic.").c_str());

            std::vector<uint8_t> bytes(count);
            for (unsigned i = 0; i < count; ++i)
                bytes[i] = static_cast<uint8_t>((raw >> (8 * i)) & 0xFF);
            return bytes;
        }

        case GattValueFormat::Utf8Text:
        {
            const std::string text = value.toString();
            return std::vector<uint8_t>(text.begin(), text.end());
        }

        case GattValueFormat::BodySensorLocation:
            // Read-only in practice, and the name-to-code direction is not one to guess at.
            DAQ_THROW_EXCEPTION(InvalidParameterException,
                                "This characteristic cannot be written by name.");

        case GattValueFormat::Hex:
            break;
    }

    return fromHex(value.toString());
}

std::string toHex(const std::vector<uint8_t>& data)
{
    static constexpr char digits[] = "0123456789ABCDEF";

    std::string hex;
    hex.reserve(data.size() * 3);

    for (size_t i = 0; i < data.size(); ++i)
    {
        if (i != 0)
            hex.push_back(' ');
        hex.push_back(digits[data[i] >> 4]);
        hex.push_back(digits[data[i] & 0x0F]);
    }

    return hex;
}

std::vector<uint8_t> fromHex(const std::string& hex)
{
    std::string digits;
    for (const char c : hex)
    {
        if (c == ' ' || c == ':' || c == '-' || c == '\t')
            continue;
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            DAQ_THROW_EXCEPTION(InvalidParameterException,
                                "Characteristic value must be hexadecimal, e.g. \"01 A2 FF\".");
        digits.push_back(c);
    }

    if (digits.size() % 2 != 0)
        DAQ_THROW_EXCEPTION(InvalidParameterException,
                            "Characteristic value must contain an even number of hex digits.");

    std::vector<uint8_t> bytes;
    bytes.reserve(digits.size() / 2);

    for (size_t i = 0; i < digits.size(); i += 2)
        bytes.push_back(static_cast<uint8_t>(std::stoul(digits.substr(i, 2), nullptr, 16)));

    return bytes;
}

std::string propertyNameFor(const std::string& uuid, std::set<std::string>& taken)
{
    auto base = ::opendaq_bt::displayLabel(uuid);

    // '.' separates a nested property path and '[' opens a list index, so openDAQ would either
    // reroute the lookup or silently truncate the name. Nothing displayLabel produces contains
    // them today; stripping is cheap insurance against a future entry in the name table.
    base.erase(std::remove_if(base.begin(),
                              base.end(),
                              [](char c) { return c == '.' || c == '[' || c == ']'; }),
               base.end());

    if (base.empty())
        base = "Characteristic";

    auto name = base;
    for (int ordinal = 2; taken.count(name) != 0; ++ordinal)
        name = base + " " + std::to_string(ordinal);

    taken.insert(name);
    return name;
}

END_NAMESPACE_OPENDAQ_BLUETOOTH

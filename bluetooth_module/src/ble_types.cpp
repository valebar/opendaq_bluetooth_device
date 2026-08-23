#include <bluetooth_module/ble/ble_types.h>

#include <algorithm>
#include <cctype>

namespace opendaq_bt::ble
{

std::string characteristicPropertiesToString(uint32_t properties)
{
    // Ordered from most to least commonly relevant so the rendered string reads naturally.
    static const struct
    {
        CharacteristicProperty flag;
        const char* name;
    } names[] = {
        {CharPropRead, "read"},
        {CharPropWrite, "write"},
        {CharPropWriteWithoutResponse, "write-without-response"},
        {CharPropNotify, "notify"},
        {CharPropIndicate, "indicate"},
        {CharPropBroadcast, "broadcast"},
        {CharPropAuthenticatedSignedWrites, "signed-write"},
        {CharPropExtendedProperties, "extended"},
    };

    std::string result;
    for (const auto& entry : names)
    {
        if ((properties & entry.flag) == 0)
            continue;

        if (!result.empty())
            result += ',';
        result += entry.name;
    }

    return result;
}

std::string normalizeUuid(const std::string& uuid)
{
    std::string result;
    result.reserve(uuid.size());

    for (const char c : uuid)
    {
        if (c == '{' || c == '}' || c == ' ' || c == '\t')
            continue;
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    return result;
}

std::string expandUuid(const std::string& uuid)
{
    const auto normalized = normalizeUuid(uuid);

    // The Bluetooth SIG base UUID; 16- and 32-bit UUIDs are shorthand for a slot within it.
    if (normalized.size() == 4)
        return "0000" + normalized + "-0000-1000-8000-00805f9b34fb";

    if (normalized.size() == 8)
        return normalized + "-0000-1000-8000-00805f9b34fb";

    return normalized;
}

}

#include <bluetooth_module/common.h>
#include <bluetooth_module/connection_string.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace opendaq_bt
{

namespace
{

bool isHexDigit(char c)
{
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

char toUpper(char c)
{
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}

/*! Strips ':' and '-' separators and uppercases, so all MAC spellings converge on one form. */
std::string hexDigitsOf(const std::string& value)
{
    std::string digits;
    digits.reserve(value.size());

    for (const char c : value)
    {
        if (c == ':' || c == '-')
            continue;
        if (!isHexDigit(c))
            return {};
        digits.push_back(toUpper(c));
    }

    return digits;
}

std::string formatMac(const std::string& twelveHexDigits)
{
    std::string mac;
    mac.reserve(17);

    for (size_t i = 0; i < 12; i += 2)
    {
        if (i != 0)
            mac.push_back(':');
        mac.push_back(twelveHexDigits[i]);
        mac.push_back(twelveHexDigits[i + 1]);
    }

    return mac;
}

/*!
 * CoreBluetooth peripheral identifiers are NSUUIDs, printed in the canonical 8-4-4-4-12 form.
 * We accept them with or without dashes and always emit the dashed uppercase form.
 */
std::string formatUuid(const std::string& thirtyTwoHexDigits)
{
    static constexpr size_t groups[] = {8, 4, 4, 4, 12};

    std::string uuid;
    uuid.reserve(36);

    size_t offset = 0;
    for (size_t g = 0; g < 5; ++g)
    {
        if (g != 0)
            uuid.push_back('-');
        uuid.append(thirtyTwoHexDigits, offset, groups[g]);
        offset += groups[g];
    }

    return uuid;
}

std::string trim(const std::string& value)
{
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};

    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

}

std::string normalizeIdentifier(const std::string& identifier)
{
    const auto digits = hexDigitsOf(trim(identifier));

    if (digits.size() == 12)
        return formatMac(digits);

    if (digits.size() == 32)
        return formatUuid(digits);

    return {};
}

std::string formatConnectionString(const std::string& identifier)
{
    const auto normalized = normalizeIdentifier(identifier);
    const auto& payload = normalized.empty() ? identifier : normalized;

    return std::string(ConnectionStringPrefix) + "://" + payload;
}

bool tryParseConnectionString(const std::string& connectionString,
                              std::string& identifierOut,
                              std::string& errorOut)
{
    const std::string scheme = std::string(ConnectionStringPrefix) + "://";

    // Schemes are case-insensitive, so compare the prefix without regard to case.
    const bool schemeMatches =
        connectionString.size() >= scheme.size() &&
        std::equal(scheme.begin(), scheme.end(), connectionString.begin(), [](char a, char b) {
            return toUpper(a) == toUpper(b);
        });

    if (!schemeMatches)
    {
        errorOut = "Connection string \"" + connectionString + "\" is not a Bluetooth connection string; expected it to start with \"" + scheme + "\".";
        return false;
    }

    const auto payload = trim(connectionString.substr(scheme.size()));
    if (payload.empty())
    {
        errorOut = "Connection string \"" + connectionString + "\" has no device identifier after \"" + scheme + "\".";
        return false;
    }

    const auto normalized = normalizeIdentifier(payload);
    if (normalized.empty())
    {
        errorOut = "\"" + payload + "\" is not a valid Bluetooth device identifier. Expected a MAC address (AA:BB:CC:DD:EE:FF) on Linux and Windows, or a CoreBluetooth peripheral UUID (8-4-4-4-12 hex digits) on macOS.";
        return false;
    }

    identifierOut = normalized;
    return true;
}

std::string identifierToLocalId(const std::string& identifier)
{
    std::string localId = "BLE_";
    localId.reserve(localId.size() + identifier.size());

    for (const char c : identifier)
        localId.push_back((c == ':' || c == '-') ? '_' : toUpper(c));

    return localId;
}

}

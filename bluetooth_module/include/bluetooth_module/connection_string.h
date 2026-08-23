#pragma once

#include <string>

namespace opendaq_bt
{

/*!
 * The connection string scheme. openDAQ's DeviceType factory takes this verbatim as the part
 * before "://", so this constant alone is what makes "bluetooth://..." route to this module.
 */
inline constexpr char ConnectionStringPrefix[] = "bluetooth";

/*!
 * Builds a "bluetooth://<identifier>" connection string. The identifier is normalised first,
 * so the string produced during discovery is byte-identical to what the parser will accept.
 */
std::string formatConnectionString(const std::string& identifier);

/*!
 * Parses "bluetooth://<identifier>" and writes the normalised identifier to @p identifierOut.
 *
 * Returns false and fills @p errorOut with a user-facing explanation when the string has the
 * wrong scheme or an identifier that is neither a MAC address nor a UUID.
 */
bool tryParseConnectionString(const std::string& connectionString,
                              std::string& identifierOut,
                              std::string& errorOut);

/*!
 * Normalises a peripheral identifier to its canonical form: MAC addresses become uppercase and
 * colon-separated ("AA:BB:CC:DD:EE:FF"), UUIDs become uppercase and dash-separated. Accepts MACs
 * written with ':' or '-' separators or with none at all, in any case.
 *
 * Returns an empty string when @p identifier is neither of those, which is the caller's signal
 * that the input is invalid.
 */
std::string normalizeIdentifier(const std::string& identifier);

/*!
 * Converts an identifier into a token usable as an openDAQ local id, which may not contain
 * the '/' character and is conventionally free of punctuation: "AA:BB:CC:DD:EE:FF" becomes
 * "BLE_AA_BB_CC_DD_EE_FF".
 */
std::string identifierToLocalId(const std::string& identifier);

}

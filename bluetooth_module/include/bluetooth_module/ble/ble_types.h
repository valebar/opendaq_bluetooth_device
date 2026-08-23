#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace opendaq_bt::ble
{

/*!
 * Bitmask of the GATT characteristic properties we care about. Values mirror the wire-level
 * GATT characteristic property bits so backends can pass native flags through with a shift-free
 * mapping where the platform exposes them numerically.
 */
enum CharacteristicProperty : uint32_t
{
    CharPropNone = 0,
    CharPropBroadcast = 1u << 0,
    CharPropRead = 1u << 1,
    CharPropWriteWithoutResponse = 1u << 2,
    CharPropWrite = 1u << 3,
    CharPropNotify = 1u << 4,
    CharPropIndicate = 1u << 5,
    CharPropAuthenticatedSignedWrites = 1u << 6,
    CharPropExtendedProperties = 1u << 7
};

/*! Renders a property bitmask as a stable, sorted, comma-separated string, e.g. "read,notify". */
std::string characteristicPropertiesToString(uint32_t properties);

/*!
 * A single BLE peripheral seen during scanning.
 *
 * @c id is the platform-native identifier and is the payload of the connection string:
 * an uppercase colon-separated MAC address on Linux and Windows, and a CoreBluetooth
 * peripheral UUID on macOS. It is therefore stable per host but NOT portable between hosts.
 */
struct Advertisement
{
    std::string id;
    std::string name;
    int16_t rssi = 0;
    std::vector<std::string> serviceUuids;
    std::map<uint16_t, std::vector<uint8_t>> manufacturerData;
    std::chrono::steady_clock::time_point lastSeen{};

    /*!
     * True when this peripheral is connected to the host adapter right now. Such a peripheral
     * stops advertising, so @c rssi is 0 -- unknown, not "very weak" -- and @c lastSeen is stamped
     * when the backend was asked rather than when a packet arrived. It is reported anyway because
     * it is connectable, and its @c serviceUuids are the resolved GATT database rather than
     * whatever fitted in an advertisement, so they are more complete than usual.
     */
    bool connected = false;
};

/*! One GATT characteristic within a service. */
struct Characteristic
{
    std::string uuid;

    /*!
     * Backend-opaque address of this characteristic, used to route reads and writes back to the
     * platform layer. It is a BlueZ D-Bus object path, a WinRT service/characteristic key, or a
     * CoreBluetooth service/characteristic UUID pair, depending on the backend. Callers must
     * treat it as an opaque token and never parse it.
     */
    std::string handle;

    uint32_t properties = CharPropNone;
};

/*! One primary or secondary GATT service and the characteristics it contains. */
struct Service
{
    std::string uuid;
    bool primary = true;
    std::vector<Characteristic> characteristics;
};

/*! Normalises a 16- or 32-bit GATT UUID shorthand to its full 128-bit form, lowercase. */
std::string expandUuid(const std::string& uuid);

/*! Lowercases a UUID and strips braces/whitespace so UUIDs from different backends compare equal. */
std::string normalizeUuid(const std::string& uuid);

}

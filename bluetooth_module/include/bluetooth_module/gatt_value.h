#pragma once

#include <bluetooth_module/common.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

BEGIN_NAMESPACE_OPENDAQ_BLUETOOTH

/*!
 * How the bytes of a characteristic are presented as an openDAQ value.
 *
 * BLE gives us raw bytes and nothing else, so a format is only claimed where the Bluetooth SIG
 * defines one unambiguously. Everything else -- every vendor characteristic, and every SIG one
 * whose layout is a structure rather than a scalar -- stays Hex, which is honest rather than
 * guessed.
 */
enum class GattValueFormat
{
    Hex,
    Utf8Text,
    Uint8,
    Uint16,
    BodySensorLocation
};

/*! The format for a characteristic UUID, in any spelling ble::expandUuid accepts. */
GattValueFormat gattValueFormat(const std::string& uuid);

CoreType gattCoreType(GattValueFormat format);

/*! The unit a format carries, or an unassigned pointer where it has none. */
UnitPtr gattUnit(GattValueFormat format);

/*! The value a property of this format holds before anything has been read. */
BaseObjectPtr gattDefaultValue(GattValueFormat format);

/*! Bytes from the peripheral to an openDAQ value. Never throws: a payload that does not fit the
 *  format degrades to its hex rendering rather than costing the caller the whole read. */
BaseObjectPtr decodeGattValue(GattValueFormat format, const std::vector<uint8_t>& raw);

/*!
 * An openDAQ value back to bytes for the peripheral.
 *
 * Throws InvalidParameterException on anything the format cannot represent -- which is what aborts
 * a bad property write before it reaches the radio.
 */
std::vector<uint8_t> encodeGattValue(GattValueFormat format, const BaseObjectPtr& value);

/*! "DE AD BE EF". */
std::string toHex(const std::vector<uint8_t>& data);

/*! Parses "DE AD BE EF", "deadbeef" or "de-ad-be-ef"; throws InvalidParameterException otherwise. */
std::vector<uint8_t> fromHex(const std::string& hex);

/*!
 * The property name a UUID gets within @p taken, which is the set of names already used in that
 * namespace -- the device for services, one service object for its characteristics.
 *
 * Property names are the addressing key now, so this has two jobs. It strips the characters
 * openDAQ reserves ('.' separates a nested path, '[' opens a list index), and it de-duplicates:
 * a peripheral may legitimately expose one service or characteristic twice, and two vendor UUIDs
 * can share the first group that shortUuid keeps. The suffix is deterministic and, because the
 * resolved GATT database is frozen for the device's lifetime, stable across reconnects.
 *
 * Inserts the result into @p taken.
 */
std::string propertyNameFor(const std::string& uuid, std::set<std::string>& taken);

END_NAMESPACE_OPENDAQ_BLUETOOTH

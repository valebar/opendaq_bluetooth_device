#pragma once

#include <string>

namespace opendaq_bt
{

/*!
 * Human-readable name for a Bluetooth SIG assigned service or characteristic UUID, or an empty
 * string when the UUID is vendor-specific. Purely cosmetic: it exists so a browsed GATT database
 * reads as "Battery Service" rather than "0000180f-0000-1000-8000-00805f9b34fb".
 */
std::string wellKnownGattName(const std::string& uuid);

/*!
 * Shortens a UUID for use in component local ids: a UUID in the Bluetooth SIG base range
 * collapses to its 16-bit form ("180F"), anything else keeps its first group ("6E400001").
 */
std::string shortUuid(const std::string& uuid);

/*!
 * The label a UUID is presented under: its assigned-numbers name if the SIG has one, otherwise the
 * shortened UUID. This is what names the device's service and characteristic properties, so it is
 * an addressing key rather than mere decoration -- see propertyNameFor in gatt_value.h, which
 * sanitises and de-duplicates it.
 */
std::string displayLabel(const std::string& uuid);

}

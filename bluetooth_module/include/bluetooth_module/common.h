#pragma once

#include <bluetooth_module/connection_string.h>

#include <opendaq/opendaq.h>

#define BEGIN_NAMESPACE_OPENDAQ_BLUETOOTH \
    namespace daq::modules::bluetooth_module \
    {

#define END_NAMESPACE_OPENDAQ_BLUETOOTH }

BEGIN_NAMESPACE_OPENDAQ_BLUETOOTH

/*! Identifier of the module itself, as reported through ModuleInfo. */
inline constexpr char ModuleName[] = "BluetoothModule";

/*! openDAQ device type id, as it appears in Instance::getAvailableDeviceTypes(). */
inline constexpr char BluetoothDeviceTypeId[] = "bluetooth_ble";

/*! Re-exported from the connection string parser, which owns the definition. */
inline constexpr const char* ConnectionStringPrefix = ::opendaq_bt::ConnectionStringPrefix;

END_NAMESPACE_OPENDAQ_BLUETOOTH

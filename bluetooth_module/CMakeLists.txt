set(LIB_NAME bluetooth_module)

# openDAQ's ModuleManager discovers modules purely by file extension. The variable that carries
# the platform's expected extension is published by openDAQ's installed CMake package config, so
# it is absent when the SDK is consumed through FetchContent -- derive it in that case.
if (NOT DEFINED OPENDAQ_MODULE_SUFFIX)
    set(OPENDAQ_MODULE_SUFFIX ".module${CMAKE_SHARED_LIBRARY_SUFFIX}")
endif()

configure_file(include/bluetooth_module/version.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/include/bluetooth_module/version.h
               @ONLY)

set(SRC_Headers
    include/bluetooth_module/ble/ble_central.h
    include/bluetooth_module/ble/ble_types.h
    include/bluetooth_module/bluetooth_device.h
    include/bluetooth_module/bluetooth_module.h
    include/bluetooth_module/common.h
    include/bluetooth_module/connection_string.h
    include/bluetooth_module/gatt_names.h
    include/bluetooth_module/gatt_value.h
    include/bluetooth_module/module_dll.h
    include/bluetooth_module/profiles/byte_reader.h
    include/bluetooth_module/profiles/channel_support.h
    include/bluetooth_module/profiles/cycling_common.h
    include/bluetooth_module/profiles/cycling_power_channel.h
    include/bluetooth_module/profiles/cycling_power_measurement.h
    include/bluetooth_module/profiles/cycling_speed_cadence_channel.h
    include/bluetooth_module/profiles/cycling_speed_cadence_measurement.h
    include/bluetooth_module/profiles/fitness_machine_channel.h
    include/bluetooth_module/profiles/gatt_profile.h
    include/bluetooth_module/profiles/heart_rate_channel.h
    include/bluetooth_module/profiles/heart_rate_measurement.h
    include/bluetooth_module/profiles/indoor_bike_data.h
    include/bluetooth_module/profiles/revolution_tracker.h
    include/bluetooth_module/reconnecting_connection.h
    include/bluetooth_module/scan_cache.h
)

set(SRC_Sources
    src/ble_types.cpp
    src/bluetooth_device.cpp
    src/bluetooth_module.cpp
    src/connection_string.cpp
    src/gatt_names.cpp
    src/gatt_value.cpp
    src/profiles/channel_support.cpp
    src/profiles/cycling_common.cpp
    src/profiles/cycling_power_channel.cpp
    src/profiles/cycling_power_measurement.cpp
    src/profiles/cycling_speed_cadence_channel.cpp
    src/profiles/cycling_speed_cadence_measurement.cpp
    src/profiles/fitness_machine_channel.cpp
    src/profiles/gatt_profile.cpp
    src/profiles/heart_rate_channel.cpp
    src/profiles/heart_rate_measurement.cpp
    src/profiles/indoor_bike_data.cpp
    src/profiles/revolution_tracker.cpp
    src/reconnecting_connection.cpp
    src/scan_cache.cpp
)

# Everything except the DLL entry point lives in an object library, so the unit tests link the
# very same objects the shipped module is built from rather than a recompiled variant.
add_library(${LIB_NAME}_objs OBJECT ${SRC_Headers} ${SRC_Sources})

target_include_directories(${LIB_NAME}_objs PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(${LIB_NAME}_objs PUBLIC daq::opendaq)

if (MSVC)
    target_compile_options(${LIB_NAME}_objs PRIVATE /bigobj)
endif()

# The platform BLE backend is part of the shipped module but not of the object library: unit
# tests substitute a fake backend and must never pull a real radio into the process.
add_library(${LIB_NAME} SHARED src/module_dll.cpp include/bluetooth_module/module_dll.h)

target_link_libraries(${LIB_NAME} PUBLIC ${LIB_NAME}_objs daq::opendaq)

# DEFINE_MODULE_EXPORTS defines daq::daqSharedLibObjectCount, whose declaration in coretypes sits
# behind this guard. It is needed by that one translation unit and nowhere else.
set_source_files_properties(src/module_dll.cpp
    PROPERTIES COMPILE_DEFINITIONS OPENDAQ_TRACK_SHARED_LIB_OBJECT_COUNT)

opendaq_bt_add_ble_backend(${LIB_NAME})

set_target_properties(${LIB_NAME} PROPERTIES
    SUFFIX ${OPENDAQ_MODULE_SUFFIX}
    DEBUG_POSTFIX ""
)

if (MSVC)
    target_compile_options(${LIB_NAME} PRIVATE /bigobj)
    target_compile_definitions(${LIB_NAME} PRIVATE OPENDAQ_MODULE_EXPORTS)
endif()

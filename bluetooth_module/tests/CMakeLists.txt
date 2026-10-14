# GoogleTest is fetched by the top-level CMakeLists, before openDAQ; see the note there.

# --- Unit tests: the openDAQ layer driven through a scripted BLE backend, no hardware ---------

add_executable(bluetooth_module_tests
    test_app.cpp
    test_bluetooth_device.cpp
    test_bluetooth_module.cpp
    test_connection_string.cpp
    test_cycling_measurements.cpp
    test_cycling_power_channel.cpp
    test_cycling_speed_cadence_channel.cpp
    test_environmental_sensing_channel.cpp
    test_fitness_machine_channel.cpp
    test_gatt_names.cpp
    test_health_thermometer_channel.cpp
    test_heart_rate_channel.cpp
    test_heart_rate_measurement.cpp
    test_multi_profile_device.cpp
    test_reconnect.cpp
    test_revolution_tracker.cpp
    test_scan_cache.cpp
    test_temperature_measurement.cpp
    fake_ble_central.h
)

target_link_libraries(bluetooth_module_tests PRIVATE bluetooth_module_objs gtest daq::opendaq)
target_include_directories(bluetooth_module_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME bluetooth_module_tests COMMAND bluetooth_module_tests)

# --- Integration test: the built shared library, loaded by a real openDAQ Instance -------------

add_executable(bluetooth_module_loading_tests test_loading_app.cpp test_module_loading.cpp)

target_link_libraries(bluetooth_module_loading_tests PRIVATE gtest daq::opendaq)
target_compile_definitions(bluetooth_module_loading_tests
    PRIVATE OPENDAQ_BT_MODULE_DIR="$<TARGET_FILE_DIR:bluetooth_module>")

# The test loads the module from disk, so the library has to exist before the test runs.
add_dependencies(bluetooth_module_loading_tests bluetooth_module)

add_test(NAME bluetooth_module_loading_tests COMMAND bluetooth_module_loading_tests)

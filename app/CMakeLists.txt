add_executable(bt_cli main.cpp)

target_link_libraries(bt_cli PRIVATE daq::opendaq)

# The CLI loads the module from disk exactly as a real application would, rather than linking it.
target_compile_definitions(bt_cli PRIVATE OPENDAQ_BT_MODULE_DIR="$<TARGET_FILE_DIR:bluetooth_module>")
add_dependencies(bt_cli bluetooth_module)

# Helpers shared by the module and application targets.

# Selects the single BLE backend source appropriate for the target platform and applies the
# platform's link requirements. Exactly one backend is ever compiled; the others are not
# added to the target at all, so they never need to parse on a foreign toolchain.
function(opendaq_bt_add_ble_backend TARGET)
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDBUS REQUIRED IMPORTED_TARGET libsystemd)

        target_sources(${TARGET} PRIVATE src/ble/ble_central_bluez.cpp src/ble/bluez_bus.cpp)
        target_link_libraries(${TARGET} PRIVATE PkgConfig::SDBUS)
        target_compile_definitions(${TARGET} PRIVATE OPENDAQ_BT_BACKEND_BLUEZ)

    elseif (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        # C++/WinRT is header-only and ships with the Windows SDK (Include/<ver>/cppwinrt/winrt) --
        # no NuGet package, no cppwinrt.exe, no .winmd step. windowsapp.lib provides the WinRT
        # activation entry points and is likewise part of the SDK.
        #
        # C++17 is enough: C++/WinRT's floor is C++17 and this backend uses no coroutines (it waits
        # on IAsyncOperation from its own apartment thread instead). This branch used to ask for
        # C++20 via a source-file COMPILE_FEATURES property, which does nothing at all --
        # COMPILE_FEATURES is a target property, so the request was silently dropped and the file
        # built at the project's CMAKE_CXX_STANDARD anyway. If a per-file flag is ever genuinely
        # needed, COMPILE_OPTIONS is the source-file property that works.
        target_sources(${TARGET} PRIVATE src/ble/ble_central_winrt.cpp)
        target_link_libraries(${TARGET} PRIVATE windowsapp)
        target_compile_definitions(${TARGET} PRIVATE OPENDAQ_BT_BACKEND_WINRT)

    elseif (CMAKE_SYSTEM_NAME MATCHES "^(Darwin|iOS)$")
        # One CoreBluetooth translation unit serves macOS and iOS; the framework, the delegate API
        # and the manager's own dispatch queue are identical on both. What differs is confined to
        # the .mm behind TARGET_OS_IPHONE (authorisation wording, the Info.plist pre-flight and the
        # suppressed power alert) and to how the host application embeds the result.
        #
        # Matching only Darwin and iOS rather than APPLE is deliberate: the FATAL_ERROR below is a
        # safety net, and tvOS, watchOS and visionOS should keep hitting it instead of quietly
        # building a backend nobody has ever run there.
        #
        # OBJCXX is enabled by the top-level CMakeLists, not here: enable_language() is only
        # valid at file scope. See the comment there.
        find_library(CORE_BLUETOOTH_FRAMEWORK CoreBluetooth REQUIRED)
        find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)

        target_sources(${TARGET} PRIVATE src/ble/ble_central_corebluetooth.mm)
        set_source_files_properties(src/ble/ble_central_corebluetooth.mm
                                    PROPERTIES LANGUAGE OBJCXX
                                               COMPILE_OPTIONS "-fobjc-arc")
        target_link_libraries(${TARGET} PRIVATE
                              ${CORE_BLUETOOTH_FRAMEWORK} ${FOUNDATION_FRAMEWORK})
        target_compile_definitions(${TARGET} PRIVATE OPENDAQ_BT_BACKEND_COREBLUETOOTH)

    elseif (CMAKE_SYSTEM_NAME STREQUAL "Android")
        # JNI headers and system libraries come from the NDK toolchain.
        target_sources(${TARGET} PRIVATE src/ble/ble_central_android.cpp src/ble/android_jni.cpp)
        target_link_libraries(${TARGET} PRIVATE log android)
        target_compile_definitions(${TARGET} PRIVATE OPENDAQ_BT_BACKEND_ANDROID)

    else()
        message(FATAL_ERROR
                "No BLE backend for CMAKE_SYSTEM_NAME='${CMAKE_SYSTEM_NAME}'. "
                "Supported: Linux, Windows, Darwin, iOS, Android.")
    endif()
endfunction()

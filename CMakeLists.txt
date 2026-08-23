cmake_minimum_required(VERSION 3.24)

set(PROJECT_VERSION_MAJOR 0)
set(PROJECT_VERSION_MINOR 1)
set(PROJECT_VERSION_PATCH 0)

project(opendaq_bluetooth_device
        VERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}
        LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Compiler extensions must stay enabled: openDAQ's object-factory macros rely on the GNU
# ',##__VA_ARGS__' comma-swallowing extension, and the SDK fails to compile under strict
# ISO mode. This setting is inherited by the fetched SDK, so it is not ours alone to tighten.
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The macOS BLE backend is Objective-C++, so that language has to be enabled -- and it has to
# happen here, at file scope in the top-level directory. enable_language() called from inside a
# function (which is where this used to live, in opendaq_bt_add_ble_backend) loads the language's
# rule variables into the function's own scope, where they are discarded on return: only the
# CMAKE_OBJCXX_COMPILER cache entry survives, so configuration looks healthy while the generator
# emits a .mm compile rule with no compile command and the link fails on the missing object file.
if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    enable_language(OBJCXX)

    # CMAKE_CXX_* does not carry over to Objective-C++; without these the .mm is compiled with the
    # compiler's default dialect rather than the one the rest of the project agrees on.
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(CMAKE_OBJCXX_EXTENSIONS ON)
endif()

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(OPENDAQ_BT_USE_SYSTEM_SDK "Use a pre-installed openDAQ SDK instead of fetching it" OFF)
option(OPENDAQ_BT_BUILD_TESTS    "Build unit tests"                                       ON)
option(OPENDAQ_BT_BUILD_APP      "Build the bt_cli smoke-test application"                ON)

# Everything lands in one directory so the openDAQ ModuleManager can be pointed at it.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(CommonUtils)

# GoogleTest must be configured before openDAQ. The SDK's CMake helpers replace the global
# include_directories() command with one that hard-errors under their "MODERN" mode, and
# GoogleTest's own build still calls it -- so it has to be added while the command is intact.
if (OPENDAQ_BT_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
        GIT_SHALLOW    TRUE
        SYSTEM
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

add_subdirectory(external)
add_subdirectory(bluetooth_module)

if (OPENDAQ_BT_BUILD_APP)
    add_subdirectory(app)
endif()

if (OPENDAQ_BT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(bluetooth_module/tests)
endif()

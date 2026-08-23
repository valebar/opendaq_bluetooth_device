set(OPENDAQ_VERSION v3.40.1)

if (OPENDAQ_BT_USE_SYSTEM_SDK)
    find_package(openDAQ 3.40 CONFIG REQUIRED)
    message(STATUS "Using pre-installed openDAQ ${openDAQ_VERSION}")
    return()
endif()

include(FetchContent)

# openDAQ's own defaults already leave every optional module, binding and streaming protocol
# OFF, so the fetched build is the core SDK plus nothing. We only need to suppress its tests --
# they roughly triple the build and pull in extra dependencies we have no use for.
set(OPENDAQ_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(OPENDAQ_ENABLE_TEST_UTILS OFF CACHE BOOL "" FORCE)

# The SDK compiles its own sources with -Werror by default, which makes any newer toolchain than
# the one it was released against fatal to our build. GCC 15 / libstdc++ 15, for instance, marks
# std::wstring_convert deprecated and openDAQ still uses it. Warnings in a vendored dependency
# are not ours to fix, so do not let them stop the build. Our own targets keep their warnings.
set(OPENDAQ_RELEASE_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(OPENDAQ_DEBUG_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(openDAQ
    GIT_REPOSITORY https://github.com/openDAQ/openDAQ.git
    GIT_TAG        ${OPENDAQ_VERSION}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    SYSTEM
)

FetchContent_MakeAvailable(openDAQ)

message(STATUS "Fetched openDAQ ${OPENDAQ_VERSION} into ${opendaq_SOURCE_DIR}")

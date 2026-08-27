#!/usr/bin/env bash
#
# Builds the openDAQ Bluetooth module for iOS and packages it as XCFrameworks an application can
# embed. This is the iOS counterpart of `cd android && gradle assembleRelease`.
#
# Four XCFrameworks come out, not one. `xcodebuild -create-xcframework` holds many slices of ONE
# library per bundle, and four dynamic libraries have to reach the application: the module itself
# plus the three openDAQ cores it links against. openDAQ builds those as shared libraries with no
# static option (core/opendaq/CMakeLists.txt hard-codes SHARED), so there is nothing to collapse.
#
# The library file inside each bundle keeps its exact basename. That matters most for
# libbluetooth_module.module.dylib: openDAQ's ModuleManager discovers modules by scanning a
# directory for that suffix and nothing else, so a rename would make the module invisible.
#
# Each SDK is a complete openDAQ + Boost cross-build, so building both takes roughly twice as long
# as one. Narrow it with --sdks=iphoneos while iterating; a release needs both.

set -euo pipefail

readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

SDKS="iphoneos,iphonesimulator"
BUILD_TYPE="Release"
DEPLOYMENT_TARGET="15.0"
ARCH="arm64"
OUTPUT_DIR="$ROOT/ios/xcframeworks"
JOBS="$(sysctl -n hw.ncpu)"
OPENDAQ_SOURCE_DIR=""
BOOST_SOURCE_DIR=""

usage()
{
    cat <<'USAGE'
Usage: ios/build-xcframeworks.sh [options]

  --sdks=LIST                Comma-separated SDKs to build. Default: iphoneos,iphonesimulator
                             Each one is a full openDAQ + Boost cross-build, so --sdks=iphoneos
                             roughly halves the time while iterating.
  --arch=ARCH                Architecture per slice. Default: arm64
  --build-type=TYPE          CMAKE_BUILD_TYPE. Default: Release
  --deployment-target=VER    Minimum iOS version. Default: 15.0
  --output-dir=PATH          Where the .xcframework bundles land.
                             Default: ios/xcframeworks
  --jobs=N                   Parallel build jobs. Default: number of CPUs
  --opendaq-source-dir=PATH  Reuse an openDAQ checkout instead of cloning it again.
                             Maps to -DFETCHCONTENT_SOURCE_DIR_OPENDAQ, the same escape hatch
                             Gradle exposes as -PopendaqSourceDir.
  --boost-source-dir=PATH    Reuse an extracted Boost tree instead of downloading the tarball.
                             Maps to -DFETCHCONTENT_SOURCE_DIR_BOOST (-PboostSourceDir on Android).
  -h, --help                 This message.

Both source directories are read-only to the build and are safe to share across slices; each SDK
still gets its own build directory, because a CMake cache cannot be re-pointed at another sysroot.
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --sdks=*)               SDKS="${1#*=}" ;;
        --arch=*)               ARCH="${1#*=}" ;;
        --build-type=*)         BUILD_TYPE="${1#*=}" ;;
        --deployment-target=*)  DEPLOYMENT_TARGET="${1#*=}" ;;
        --output-dir=*)         OUTPUT_DIR="${1#*=}" ;;
        --jobs=*)               JOBS="${1#*=}" ;;
        --opendaq-source-dir=*) OPENDAQ_SOURCE_DIR="${1#*=}" ;;
        --boost-source-dir=*)   BOOST_SOURCE_DIR="${1#*=}" ;;
        -h|--help)              usage; exit 0 ;;
        *) echo "error: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

IFS=',' read -r -a SDK_LIST <<< "$SDKS"

# XCFramework bundle name : glob matching the library inside the build's bin/ directory.
#
# The openDAQ libraries are globbed rather than spelled out. Their names carry the pointer width
# and the SDK major version -- libopendaq-64-3.dylib today -- and both are derived, not fixed. A
# glob with a one-match assertion fails loudly on an SDK bump instead of silently packaging
# nothing.
readonly LIBRARIES=(
    "OpenDAQCoreTypes:libdaqcoretypes-*.dylib"
    "OpenDAQCoreObjects:libdaqcoreobjects-*.dylib"
    "OpenDAQ:libopendaq-*.dylib"
    "OpenDAQBluetoothModule:libbluetooth_module.module.dylib"
)

fail() { echo "error: $*" >&2; exit 1; }

# --- build one slice per SDK -------------------------------------------------------------------

for sdk in "${SDK_LIST[@]}"; do
    build_dir="$ROOT/build-ios-$sdk"
    echo "==> Configuring $sdk ($ARCH, iOS $DEPLOYMENT_TARGET) in $build_dir"

    cmake_args=(
        -S "$ROOT" -B "$build_dir" -G Ninja
        -DCMAKE_SYSTEM_NAME=iOS
        -DCMAKE_OSX_SYSROOT="$sdk"
        -DCMAKE_OSX_ARCHITECTURES="$ARCH"
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET"
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
        # Passed explicitly even though the root CMakeLists already defaults them off for iOS,
        # exactly as android/build.gradle.kts does for Android.
        -DOPENDAQ_BT_BUILD_TESTS=OFF
        -DOPENDAQ_BT_BUILD_APP=OFF
    )
    [ -n "$OPENDAQ_SOURCE_DIR" ] &&
        cmake_args+=(-DFETCHCONTENT_SOURCE_DIR_OPENDAQ="$OPENDAQ_SOURCE_DIR")
    [ -n "$BOOST_SOURCE_DIR" ] &&
        cmake_args+=(-DFETCHCONTENT_SOURCE_DIR_BOOST="$BOOST_SOURCE_DIR")

    cmake "${cmake_args[@]}"

    # Building the module target alone is the analogue of AGP's `targets += "bluetooth_module"`.
    # It pulls in the three openDAQ dylibs transitively while skipping the SDK's mock library,
    # which is tens of megabytes of test scaffolding nothing here links against.
    echo "==> Building $sdk"
    cmake --build "$build_dir" --target bluetooth_module -j "$JOBS"
done

# --- check what came out -----------------------------------------------------------------------

resolve_library()
{
    # Echoes the single file in <build>/bin matching a glob, or fails.
    local build_dir="$1" glob="$2"
    local matches=()
    while IFS= read -r line; do matches+=("$line"); done \
        < <(find "$build_dir/bin" -maxdepth 1 -name "$glob" | sort)

    [ "${#matches[@]}" -eq 1 ] ||
        fail "expected exactly one '$glob' in $build_dir/bin, found ${#matches[@]}"
    echo "${matches[0]}"
}

expected_platform()
{
    case "$1" in
        iphoneos)        echo "IOS" ;;
        iphonesimulator) echo "IOSSIMULATOR" ;;
        *) fail "unknown SDK '$1'" ;;
    esac
}

check_library()
{
    local path="$1" sdk="$2"
    local name platform
    name="$(basename "$path")"
    platform="$(expected_platform "$sdk")"

    lipo -info "$path" | grep -q "$ARCH" ||
        fail "$name is not $ARCH"

    xcrun vtool -show-build "$path" | grep -qE "^ *platform +$platform\$" ||
        fail "$name is not built for $platform"

    xcrun vtool -show-build "$path" | grep -qE "^ *minos +$DEPLOYMENT_TARGET\$" ||
        fail "$name does not target iOS $DEPLOYMENT_TARGET"

    otool -D "$path" | grep -q "^@rpath/$name\$" ||
        fail "$name has no @rpath install name"

    # Exactly one runpath, and a relative one. An absolute build-tree path here would mean
    # CMAKE_BUILD_WITH_INSTALL_RPATH stopped taking effect; see the note in the root CMakeLists.
    local rpaths
    rpaths="$(otool -l "$path" | awk '/LC_RPATH/{f=1} f&&/^ *path /{print $2; f=0}')"
    [ "$rpaths" = "@loader_path" ] ||
        fail "$name has runpath '$rpaths', expected exactly '@loader_path'"
}

for sdk in "${SDK_LIST[@]}"; do
    build_dir="$ROOT/build-ios-$sdk"
    echo "==> Checking $sdk output"

    for entry in "${LIBRARIES[@]}"; do
        lib="$(resolve_library "$build_dir" "${entry#*:}")"
        check_library "$lib" "$sdk"
    done

    module="$(resolve_library "$build_dir" "libbluetooth_module.module.dylib")"

    otool -L "$module" | grep -q "CoreBluetooth.framework/CoreBluetooth" ||
        fail "the module does not link CoreBluetooth"

    # The entry points DEFINE_MODULE_EXPORTS produces. Without them ModuleManager loads the dylib
    # and then rejects it.
    for symbol in _createModule _checkDependencies _getCoreVersionMetadata; do
        nm -gU "$module" | grep -q " $symbol\$" ||
            fail "the module does not export $symbol"
    done
done

# --- assemble ----------------------------------------------------------------------------------

mkdir -p "$OUTPUT_DIR"

for entry in "${LIBRARIES[@]}"; do
    name="${entry%%:*}"
    glob="${entry#*:}"
    output="$OUTPUT_DIR/$name.xcframework"

    args=()
    for sdk in "${SDK_LIST[@]}"; do
        args+=(-library "$(resolve_library "$ROOT/build-ios-$sdk" "$glob")")
    done

    # create-xcframework refuses to write over an existing bundle.
    rm -rf "$output"
    xcodebuild -create-xcframework "${args[@]}" -output "$output" > /dev/null

    # The basename has to survive into the bundle: ModuleManager finds the module by its
    # ".module.dylib" suffix and by nothing else.
    expected="$(basename "$(resolve_library "$ROOT/build-ios-${SDK_LIST[0]}" "$glob")")"
    for index in "${!SDK_LIST[@]}"; do
        actual="$(plutil -extract "AvailableLibraries.$index.LibraryPath" raw \
                          "$output/Info.plist")"
        [ "$actual" = "$expected" ] ||
            fail "$name.xcframework renamed its library to '$actual' (expected '$expected')"
    done

    echo "==> $output"
done

cat <<DONE

Done. ${#LIBRARIES[@]} XCFrameworks in $OUTPUT_DIR, each carrying: ${SDKS}

Consume them by adding ios/ as a local Swift package, or by dragging all four bundles into the
application target's "Frameworks, Libraries, and Embedded Content" with "Embed & Sign".

All four must be embedded. The application never references the module symbolically -- openDAQ
dlopens it -- so it is easy to leave out by mistake, and discovery then silently finds nothing.
DONE

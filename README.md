# openDAQ Bluetooth Low Energy (BLE) device module

Connect **Bluetooth Low Energy (BLE) sensors and devices to
[openDAQ](https://opendaq.com)** on Linux, Windows, macOS, and Android. This cross-platform C++
openDAQ device module adds Bluetooth discovery, GATT access, measurement signals, smart-trainer
control, and automatic reconnection through openDAQ's standard device API.

Use it to bring Bluetooth heart-rate sensors, cycling power meters, speed and cadence sensors, and
FTMS indoor trainers into an openDAQ data-acquisition application. Nearby BLE peripherals appear
in normal openDAQ device discovery and connect with a `bluetooth://...` connection string, so an
application does not need to call BlueZ, C++/WinRT, CoreBluetooth, or `android.bluetooth` directly.

## openDAQ Bluetooth features

- Discover and connect to Bluetooth Low Energy peripherals through an openDAQ `Instance`.
- Decode Bluetooth SIG Heart Rate, Cycling Power, Cycling Speed and Cadence, and Fitness Machine
  Service (FTMS) data into timestamped openDAQ signals.
- Browse any connected device's GATT services and characteristics as openDAQ properties.
- Read and write BLE GATT characteristics, including vendor-specific characteristics.
- Control compatible Bluetooth smart trainers in ERG, resistance, and simulation modes.
- Reconnect automatically without replacing existing openDAQ channels, signals, or stream readers.
- Use native Bluetooth backends on Linux (BlueZ), Windows (C++/WinRT), and macOS
  (CoreBluetooth).

## Quick start: discover and connect to a Bluetooth device

After building the module and making it available to the openDAQ ModuleManager, discover nearby
Bluetooth devices and connect using the returned connection string:

```cpp
auto instance = Instance();

for (const auto& info : instance.getAvailableDevices())
    std::cout << info.getName() << "  " << info.getConnectionString() << "\n";
//   Example HR Sensor       bluetooth://02:00:00:00:00:01

auto device = instance.addDevice("bluetooth://02:00:00:00:00:01");
```

By default discovery lists only peripherals advertising a service this module has a profile for —
one supported heart-rate strap rather than the 88 assorted phones, TVs and earbuds the same adapter
can hear. Set
[`FilterByKnownProfiles`](#module-options) to `false` to list everything; a peripheral hidden from
the list is still connectable by connection string either way.

A peripheral this host is already connected to is listed as well, even though a connected
peripheral stops advertising. Its `Rssi` reads 0 because there is no measurement to report — its
`Connected` property is what tells that apart from a very strong signal, and `bt_cli scan` prints
`-` in the RSSI column for it. The services it is filtered on are then its *resolved* GATT
database rather than an advertisement, so with the filter off a connected keyboard or mouse shows
up too.

The connected device *is* the peripheral's GATT database: one property per service, and inside it
one property per characteristic whose value is the characteristic's value.

```
- Generic Access                                    [00001800-0000-1000-8000-00805f9b34fb]
  - Device Name              = Example HR Sensor   (00002a00-… | read)
- Heart Rate                                        [0000180d-0000-1000-8000-00805f9b34fb]
  - Heart Rate Measurement                          (00002a37-… | notify)
- Device Information                                [0000180a-0000-1000-8000-00805f9b34fb]
  - Manufacturer Name        = Example Devices     (00002a29-… | read)
  - Model Number             = HR-1                 (00002a24-… | read)
- Battery Service                                   [0000180f-0000-1000-8000-00805f9b34fb]
  - Battery Level            = 75                   (00002a19-… | read,notify)
```

So a characteristic is addressed the way everything else in openDAQ is — by name:

```cpp
device.getPropertyValue("Battery Service.Battery Level");     // 75, an integer
device.setPropertyValue("Nordic UART.Nordic UART RX", "01 A2");
```

## Supported openDAQ Bluetooth measurement signals

A peripheral implementing a GATT service this module understands gets an openDAQ **channel**
carrying decoded signals, alongside the raw GATT database. Four profiles are implemented:

| Service | Channel | What it is |
| --- | --- | --- |
| Heart Rate (0x180D) | `HeartRate` | chest straps and wrist sensors |
| Cycling Power (0x1818) | `CyclingPower` | crank, pedal and hub power meters |
| Cycling Speed and Cadence (0x1816) | `CyclingSpeedCadence` | magnet-and-pod speed/cadence sensors |
| Fitness Machine (0x1826) | `FitnessMachine` | smart indoor trainers, including control |

That registry is also what discovery filters on, so adding a profile widens the device list as
well as producing channels. A smart trainer typically hosts three of these at once and gets three
channels; each has its own signal namespace, so `CyclingPower/WheelRevolutions` and
`CyclingSpeedCadence/WheelRevolutions` coexist.

```
BluetoothDevice
├── properties
│   ├── Address, Name, Connected, MtuSize
│   └── <one object property per GATT service, holding one property per characteristic>
└── IO
    ├── HeartRate                (Channel)
    │   ├── HeartRate       [bpm]
    │   ├── RrInterval      [s]
    │   └── EnergyExpended  [kJ]
    ├── CyclingPower             (Channel)
    │   ├── InstantaneousPower   [W]     PedalPowerBalance [%]
    │   ├── AccumulatedTorque    [N.m]   AccumulatedEnergy [kJ]
    │   ├── Cadence              [rpm]   CrankRevolutions  [rev]
    │   ├── Speed                [km/h]  Distance          [m]    WheelRevolutions [rev]
    │   └── MaximumForce / MinimumForce / MaximumTorque / MinimumTorque /
    │       MaximumAngle / MinimumAngle / TopDeadSpotAngle / BottomDeadSpotAngle
    ├── CyclingSpeedCadence      (Channel)
    │   ├── Cadence         [rpm]   CrankRevolutions [rev]
    │   └── Speed           [km/h]  Distance         [m]   WheelRevolutions [rev]
    └── FitnessMachine           (Channel)
        ├── Speed / AverageSpeed          [km/h]
        ├── Cadence / AverageCadence      [rpm]
        ├── InstantaneousPower / AveragePower  [W]
        ├── TotalDistance [m]  ResistanceLevel  HeartRate [bpm]  MetabolicEquivalent
        ├── TotalEnergy / EnergyPerHour / EnergyPerMinute  [kcal]
        └── ElapsedTime / RemainingTime   [s]
```

These are **asynchronous** signals: the domain uses an explicit rule, so every sample carries its
own timestamp rather than being placed by a sample rate. This matches how GATT
notifications, which arrive when the peripheral chooses — in practice about once a second, but
visibly irregular. This output is synthetic:

```
$ bt_cli monitor bluetooth://02:00:00:00:00:01 25
  t=1700000000000000us  HeartRate/HeartRate 60 bpm
  t=1700000001000000us  HeartRate/HeartRate 61 bpm
  t=1700000002000000us  HeartRate/HeartRate 60 bpm
```

Each value signal has its **own** domain signal, because their per-notification sample counts
differ: there is always exactly one heart rate reading, but zero-or-many RR intervals and an
energy figure only when the peripheral reports one. The cycling profiles are the same story —
every optional field is gated by a flag bit per notification, and the Fitness Machine service
explicitly allows one data record to be split across several notifications. Signals sharing a
domain must agree on sample count, so they cannot be merged.

RR intervals are the beat-to-beat timings behind HRV. They are back-dated — the newest beat lands
at the arrival time and earlier ones are placed by walking the interval sum backwards — because BLE
carries no device-side timestamps, so host arrival time is the best timing available.

### Cadence, speed and distance are derived

Neither cycling service transmits a rate. Both transmit a cumulative revolution counter and the
timestamp of the last revolution, and the host differentiates. Three consequences:

- **The first notification after connecting produces no cadence or speed** — there is nothing to
  differentiate it against yet.
- **Speed and distance need a wheel size.** Each cycling channel has a writable
  `WheelCircumferenceMm` property, defaulting to 2096 (a 700×25c tyre). A device exposing both
  cycling services has two of them; set both.
- **`Cadence`, `Distance` and the revolution counters count from the channel's first
  notification**, not from the sensor's own counter — which runs from whenever its battery went in,
  and whose crank half is only 16 bits wide, wrapping about every four hours of riding.

A sensor that stops turning emits one explicit zero after three seconds and then goes quiet, so a
plot drops to zero rather than freezing at the last cadence you pedalled.

### Driving a smart trainer

Where the trainer exposes the Fitness Machine Control Point (0x2AD9), the `FitnessMachine` channel
gains actions. Each appears only when the trainer's Target Setting Features say it is accepted —
an always-present button that always fails would be worse than no button:

| Action | Needs |
| --- | --- |
| `RequestControl()`, `Reset()`, `Start()`, `Stop()`, `Pause()` | the control point |
| `SetTargetPower(Watts)` — ERG mode | Power Target support |
| `SetTargetResistanceLevel(Level)` | Resistance Target support |
| `SetSimulationParameters(WindSpeedMps, GradePercent, Crr, Cw)` | Indoor Bike Simulation support |

Most trainers want `RequestControl()` before they will honour anything else.

**Control is asynchronous.** An action writes its request and returns; the trainer's verdict
arrives later as an indication and lands in the read-only `LastControlResult` property, as
`"Set Target Power: Success"` or `"Set Target Power: Control Not Permitted"`. Watch that property
— a request another application has taken control of the trainer away from looks exactly like one
that worked. A target outside the machine's published `MinTargetPowerW`/`MaxTargetPowerW` is
rejected before anything is written, so that failure at least is synchronous.

**None of the cycling profiles has been run against real hardware.** They are covered by unit
tests over the full flag matrix of each wire format, and the heart rate profile they are modelled
on is verified against a live strap — but no trainer, power meter or cadence sensor has been
connected to this module. The control point in particular exercises characteristic **writes**,
which are themselves unexercised on macOS. Cross-check any reading against the device's own head
unit before trusting it: a resolution error in these formats can still produce plausible numbers.

Each channel also exposes descriptive read-only properties — `SensorLocation` and `Features` on the
cycling channels, `Features`, `TargetSettingFeatures`, `TrainingStatus` and the supported power and
resistance ranges on the Fitness Machine one — plus, for heart rate, `SensorContactSupported`,
`SensorContactDetected`, `BodySensorLocation` and a `ResetEnergyExpended` action where the
peripheral implements the Heart Rate Control Point (0x2A39).

A peripheral without a recognised service simply gets no channels and behaves exactly as before,
and so does one whose service is present but unusable — a Fitness Machine that is a treadmill
rather than an indoor bike, say. Adding another profile means one entry in the registry in
`bluetooth_module/src/profiles/gatt_profile.cpp`, plus the decoder and channel behind it — no
change to the device, the discovery path or the backends.

## Platform support for openDAQ Bluetooth

| Platform | Backend | State |
| --- | --- | --- |
| Linux | BlueZ over D-Bus (sd-bus) | **Built and verified against real hardware** |
| Windows | C++/WinRT | **Compiles and loads (ARM64, MSVC)** — not yet run against a radio, see below |
| macOS | CoreBluetooth (Objective-C++) | **Built and verified against real hardware** (arm64, macOS 15) |
| iOS | CoreBluetooth (Objective-C++) — the same backend source as macOS | **Cross-compiles for device arm64 and simulator arm64 (iOS 15+, Xcode 26)**; never loaded into an application, never run against a radio, see below |
| Android | JNI over `android.bluetooth` | **Cross-compiles for arm64-v8a and armeabi-v7a (NDK 27+)**; arm64-v8a loads and passes its suites on a physical device, armeabi-v7a has never been executed — never run against a radio, see below |

On macOS, discovery, connecting, GATT resolution, characteristic reads and heart-rate notification
streaming have all been run against a live peripheral. Characteristic **writes** and **two
peripherals connected at the same time** are implemented and compile, but have not been exercised
against real hardware yet.

The hardware verified on any platform is a heart rate strap. **The cycling power, speed/cadence
and fitness machine profiles have never been connected to a real device** — they are covered by
unit tests over the complete flag matrix of each wire format, and they share the connection,
reconnection and signal machinery the heart rate profile exercises, but no trainer, power meter or
cadence sensor has been on the other end. Trainer control additionally depends on the
characteristic-write path, unexercised on macOS and Windows.

The Windows backend has now been through a compiler. It builds clean for ARM64 with MSVC 14.4x
against the Windows 10.0.26100 SDK (`/W4 /permissive-`, no warnings), links, exports the module
entry points, and is loaded by a real openDAQ `ModuleManager`; `isAvailable()` and discovery were
exercised on a host with no adapter and behave correctly. The unit tests and the module-loading
tests passed there at the time of that bring-up; the reconnection tests added since have not been
run on Windows.

What it has **not** had is a radio: the machine it was brought up on has no Bluetooth adapter at
all. Scanning, connecting, GATT browsing and heart-rate streaming remain unexercised on Windows,
and x64 has not been compiled (only ARM64). These paths should be considered unverified.

Bringing it up needed rather more than fixing build errors, and the fixes are worth knowing about
because most of them are about *threading*, not about WinRT syntax:

- **The backend now owns a dedicated MTA apartment thread** and marshals every WinRT call, event
  registration/revocation and WinRT object release onto it. It previously called
  `init_apartment(multi_threaded)` behind a `std::call_once`, which put only the *first* caller
  into an apartment and swallowed `RPC_E_CHANGED_MODE` — so an STA host, or simply a second
  calling thread, silently got a blocking wait it must not make. See *Platform notes* below.
- **`winrt::hresult_error` is translated to `BleError`** at that one choke point. It does not
  derive from `std::exception`, so every `catch (const std::exception&)` above the backend was
  blind to it — including the one in `HeartRateChannel`'s destructor, where an escaping WinRT
  error means `std::terminate()` on the very ordinary path of a peripheral going out of range.
- **No WinRT delegate captures a raw `this`.** WinRT does not promise that revoking an event waits
  for handlers already running, and `BluetoothLEAdvertisementWatcher.Received` is documented to be
  able to fire after `Stop()`. Handler state now lives in `shared_ptr` blocks (`ConnectionState`,
  `ScanState`), the same shape the CoreBluetooth backend already used for the same reason.
- `connect()` honours its `timeout` as a per-phase budget, matching BlueZ and CoreBluetooth; the
  GATT operations gained a ceiling instead of blocking forever inside a property handler.
- The `GattSession` is retained with `MaintainConnection(true)` instead of being created and
  dropped one line later, so Windows keeps the link up and the MTU is read after negotiation
  rather than before. `GattDeviceService` objects are retained and `Close()`d for the same reason.
- `stopScan()` no longer revokes an event while holding the lock its handler takes; `subscribe()`
  no longer leaks its `ValueChanged` registration when the CCCD write fails; the `volatile bool`
  cross-thread flags became `std::atomic<bool>` (`volatile` carries no ordering at all under
  MSVC's ARM64 default of `/volatile:iso`); `parseAddress` rejects a macOS-style UUID instead of
  mangling it into a plausible 48-bit address; and the missing
  `<winrt/Windows.Devices.Radios.h>` include — the file's only actual compile error — is in.

Android uses JNI and a Java shim because the NDK has no Bluetooth API, so it ships as an **AAR**.
The module and openDAQ v3.40.1 build unpatched (minSdk 26) for both ABIs the AAR carries,
`arm64-v8a` and `armeabi-v7a`.

The two ABIs are **not** equally verified, and the difference matters:

- **`arm64-v8a`** loads successfully and passes all 238 unit tests and 5 module-loading tests,
  both on an arm64 emulator and on a physical Pixel 8a running Android 17.
- **`armeabi-v7a` has never been executed at all.** It compiles, links and packages, and the
  binary has been checked to be genuine 32-bit ARM with no text relocations — but no 32-bit
  binary from this project has ever run. Apple Silicon has no AArch32 support and current arm64
  emulator images carry no 32-bit userspace, so exercising it needs a physical device with 32-bit
  support.

**Radio-dependent behavior and permission flows remain unverified on Android**, on either ABI.

iOS reuses the macOS backend **unchanged**. `ble_central_corebluetooth.mm` is one translation unit
serving both Apple platforms, and the only conditionals in it are two reason strings, the
Info.plist pre-flight and the suppressed power alert. What iOS needed was a build system, packaging and a permissions story — not a second
backend.

The module cross-compiles for `arm64` against the device and simulator SDKs and ships as four
XCFrameworks (see *Build for iOS*). openDAQ v3.40.1 and Boost 1.82 cross-compile unpatched, but
only once `CMAKE_SYSTEM_PROCESSOR` is supplied: CMake leaves it empty for an Apple cross-build, and
without it Boost.Context configures itself to assemble x86-64 for an arm64 target. The top-level
`CMakeLists.txt` derives it from `CMAKE_OSX_ARCHITECTURES`.

**Nothing about iOS has been run.** The build script checks that each library is `arm64`, targets
the right platform at the right minimum version, carries an `@rpath` install name, resolves its
siblings through `@loader_path` and exports the module entry points — but no application has
embedded them, no `dlopen` has been attempted, and no radio has been involved. The Simulator cannot
close that gap: it has no Bluetooth hardware at all, and `isAvailable()` reports
`CBManagerStateUnsupported` there by design.

## Build the openDAQ Bluetooth module

Requires CMake ≥ 3.24 and a C++17 compiler. On Linux also `libsystemd-dev` (for sd-bus) and a
running `bluetooth` service.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j "$(nproc)"
```

The first configure fetches openDAQ v3.40.1 and GoogleTest; building the SDK takes roughly fifteen
minutes. To use an SDK you already have, pass `-DOPENDAQ_BT_USE_SYSTEM_SDK=ON`, or point
`-DFETCHCONTENT_SOURCE_DIR_OPENDAQ=/path/to/opendaq` at an existing checkout.

The module lands in `build/bin/` as `libbluetooth_module.module.so` (`.dll` / `.dylib` elsewhere).
Point an openDAQ application at that directory, or set `OPENDAQ_MODULES_PATH`.

On **Windows/ARM64** the top-level `CMakeLists.txt` corrects `CMAKE_SYSTEM_PROCESSOR`, and
`cmake/CMakeASM_ARMASMLinkerInformation.cmake` supplies a module CMake 3.31 requires and Boost
1.82 does not ship. Without them openDAQ v3.40.1 does not build there: RapidJSON pulls in SSE2
intrinsics and Boost.Context tries to assemble x86 with MASM. No extra flags are needed — see
*Windows on ARM64* in `DEVELOPMENT.md` for why.

### Build for Android

Android produces an **AAR**, because the backend needs Java classes alongside the native library
(see *Platform support* above). It requires the Android SDK, NDK 27+, JDK 17+, and Gradle 8.13+.

```bash
cd android && gradle assembleRelease
```

The AAR is written to `android/build/outputs/aar/opendaq-bluetooth-release.aar` and carries both
`arm64-v8a` and `armeabi-v7a`.

**A cold build compiles openDAQ and Boost once per ABI**, so the default two-ABI build takes
roughly twice as long as a single-ABI one. Narrow it while iterating:

```bash
cd android && gradle assembleRelease -PabiFilters=arm64-v8a
```

`-PabiFilters` takes a comma-separated list and defaults to `arm64-v8a,armeabi-v7a`, so a release
AAR is complete unless you ask otherwise. `-PopendaqSourceDir=...` and `-PboostSourceDir=...`
reuse local sources and are shared safely across ABIs — the FetchContent source trees are
read-only, and each ABI still gets its own build directory.

The `armeabi-v7a` half of the AAR contains `libopendaq-32-3.so` where the `arm64-v8a` half has
`libopendaq-64-3.so`; openDAQ names its libraries after pointer width. That is expected, and
nothing resolves those names by hand.

The project has no committed Gradle wrapper; run `gradle wrapper` locally if needed.

To build test executables for a device — a CMake cache cannot be re-pointed at another ABI, so
each ABI needs its own build directory:

```bash
cmake -S . -B build-android-arm64-v8a -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
      -DCMAKE_BUILD_TYPE=Release -DOPENDAQ_BT_BUILD_TESTS=ON
cmake --build build-android-arm64-v8a -j "$(nproc)"

cmake -S . -B build-android-armeabi-v7a -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI=armeabi-v7a -DANDROID_PLATFORM=android-26 \
      -DCMAKE_BUILD_TYPE=Release -DOPENDAQ_BT_BUILD_TESTS=ON
cmake --build build-android-armeabi-v7a -j "$(nproc)"
```

Tests and the CLI default to off on Android. Push the explicitly enabled tests with `adb`, from
the build directory matching the device's ABI (`adb shell getprop ro.product.cpu.abi`):

```bash
adb push build-android-arm64-v8a/bin /data/local/tmp/odbt
adb shell 'cd /data/local/tmp/odbt && LD_LIBRARY_PATH=. \
           OPENDAQ_BT_MODULE_DIR=/data/local/tmp/odbt ./bluetooth_module_loading_tests'
```

Running the `armeabi-v7a` binaries needs a physical device with 32-bit userspace support. Apple
Silicon cannot execute them at all, and current arm64 emulator images ship no 32-bit runtime.

Use `llvm-strip --strip-unneeded` before pushing large debug builds.

### Build for iOS

iOS produces four **XCFrameworks**, because four dynamic libraries have to reach the application:
the module and the three openDAQ cores it links against. `xcodebuild -create-xcframework` holds
many slices of one library per bundle, and openDAQ builds its cores as shared libraries with no
static option, so there is nothing to collapse into fewer.

Requires Xcode 15+ (developed against Xcode 26.6 and the iOS 26.5 SDK), CMake ≥ 3.24, Ninja, and
`mono` on `PATH` — openDAQ runs RTGen, a .NET code generator, on the host even when
cross-compiling.

```bash
ios/build-xcframeworks.sh
```

The bundles land in `ios/xcframeworks/`, each carrying an `ios-arm64` and an `ios-arm64-simulator`
slice, alongside an `ios/Package.swift` that vends all four as one Swift package product.

**Each SDK is a full openDAQ + Boost cross-build**, so building both takes roughly twice as long as
one. Narrow it while iterating:

```bash
ios/build-xcframeworks.sh --sdks=iphoneos
```

`--opendaq-source-dir=` and `--boost-source-dir=` reuse local sources instead of fetching them, and
are the exact counterparts of Gradle's `-PopendaqSourceDir` and `-PboostSourceDir`: both trees are
read-only to the build and safe to share across slices, while each slice still gets its own build
directory. `--help` lists the rest.

To drive CMake directly — one build directory per SDK, because a cache cannot be re-pointed at
another sysroot:

```bash
cmake -S . -B build-ios-iphoneos -G Ninja \
      -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_OSX_SYSROOT=iphoneos \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build-ios-iphoneos --target bluetooth_module -j "$(sysctl -n hw.ncpu)"
```

Pass `-DCMAKE_OSX_SYSROOT=iphonesimulator` for the simulator slice; CMake defaults to `iphoneos`.
Name exactly one architecture per build directory — the top-level `CMakeLists.txt` rejects a fat
`CMAKE_OSX_ARCHITECTURES`, because it derives `CMAKE_SYSTEM_PROCESSOR` from it and a fat build has
no single answer to give. See *iOS* in `DEVELOPMENT.md` for why that variable has to be derived at
all.

Tests and the CLI default to off on iOS and cannot usefully be turned on: nothing in `bin/` can be
executed, and CMake forces every iOS executable into a `.app` bundle.

## Bluetooth connection strings

```
bluetooth://AA:BB:CC:DD:EE:FF                        Linux, Windows
bluetooth://E4C7A1B2-3D45-4F6A-8B9C-0D1E2F3A4B5C     macOS
```

**Connection strings are specific to the host that produced them.** Apple's CoreBluetooth
deliberately never exposes a peripheral's hardware address; it hands out a UUID that it generates
per Mac. So a string copied from a Mac will not resolve on Linux, and vice versa. Discover devices
on the machine that will connect to them.

Addresses are accepted with `:` or `-` separators or none at all, in any case, and are normalised
to the uppercase colon-separated form. The scheme itself is case-insensitive.

A peripheral must have been *heard* — or be connected to this host — before it can be connected
to; call `getAvailableDevices()` first. It does not have to have been *listed*: the profile filter
narrows the returned list only, and a peripheral it hides is connectable by connection string with
its advertised name intact. BLE peripherals using private resolvable addresses rotate them every
few minutes, so a connection string can go stale; re-run discovery to get the current one.

Connecting to a peripheral another application already has a link to works — the platform shares
the link — but removing the openDAQ device disconnects it, which takes the peripheral away from
that application too.

A peripheral's service list often arrives in a scan response a moment after its first
advertisement, so a device can be missing from one `getAvailableDevices()` call and present in the
next. Run discovery more than once, or give `ScanWarmupMs` room.

## Bluetooth disconnection and automatic reconnection

A BLE peripheral wanders out of range; that is normal, not a fault. When the link drops the device
goes to `Reconnecting` and the module retries indefinitely in the background, and when the
peripheral comes back the device returns to `Connected` on its own.

The reconnection is deliberately transparent to everything above it:

- **The same channels, the same signals.** A `StreamReader` attached before the outage keeps
  working through it. It produces nothing while the link is down, then resumes — the gap shows up
  as a jump in the domain timestamps, which is exactly what an asynchronous signal is for. Nothing
  is rebuilt, so no `SignalPtr` a consumer holds goes stale.
- **The same property names.** They are derived once from the database the first connection
  resolved, so a path you address stays valid even when the peripheral comes back with its backend
  handles renumbered underneath; the module translates.
- Reading a property while the link is down returns the last known value rather than throwing, so
  browsing a disconnected device keeps working. Writing throws. Every readable characteristic is
  re-read once the link is back.

Four things worth knowing:

- On Linux the reconnector asks the backend to scan before each attempt. BlueZ drops a non-bonded
  peripheral from its object tree once it stops being heard, and `connect()` then fails forever
  unless something is scanning. The consequence is that a device that is reconnecting keeps the
  adapter scanning until it succeeds or goes away.
- A peripheral that comes back with a *changed* GATT database is not re-modelled: services that
  appeared are not added, and a subscription whose characteristic is gone is dropped with a note in
  the status message. Remove and re-add the device for that.
- If `bluetoothd` itself dies, BlueZ never reports the link as down, so the drop is not noticed at
  all. That is a gap in the Linux backend, not in the reconnector.
- Reconnection has been exercised against a radio on Linux only.

## Configure Bluetooth discovery and connections

| Property | Default | Scope | Meaning |
| --- | --- | --- | --- |
| `ConnectionTimeoutMs` | 15000 | per-device | Budget for establishing the link, and again for resolving the GATT database. It applies to each phase separately, not to the total. Also the budget for each reconnection attempt. |
| `AutoReconnect` | `true` | per-device | Re-establish the link by itself when it drops, indefinitely. Set `false` for the older behaviour: the device goes to `Unrecoverable` and stays there until you remove and re-add it. |
| `ReconnectIntervalMs` | 5000 | per-device | Gap between reconnection attempts. A *failing* attempt already costs up to `2 x ConnectionTimeoutMs` on Linux, so at the defaults the real cadence is nearer 35 s than 5 s; lower `ConnectionTimeoutMs`, not this, if you need attempts closer together. |
| `FilterByKnownProfiles` | `true` | module option | Discovery reports only peripherals advertising — or, if connected to this host, exposing — a service this module has a profile for: heart rate, cycling power, cycling speed and cadence, or fitness machine. Peripherals advertising no services at all are hidden too. Set `false` to list everything the adapter can hear or is connected to. |
| `ScanWarmupMs` | 2000 | module default | How long the *first* `getAvailableDevices()` blocks so the radio has a chance to hear something. Later calls never block. |
| `DeviceTimeoutMs` | 30000 | module default | A peripheral not heard from for this long drops out of discovery. It does not apply to a peripheral connected to this host, which has stopped advertising and would otherwise always time out; that one leaves the list when its link drops. |

**Per-device** keys are overridable through `instance.addDevice(connectionString, config)`.
`ConnectionTimeoutMs`, `AutoReconnect` and `ReconnectIntervalMs` are live on that path — the others
are read when the module is constructed, and openDAQ's ModuleManager constructs modules without a
config, so passing them to `addDevice` does nothing.

### Module options

Module-level settings reach the module through openDAQ's config providers, under
`Modules` → `BluetoothModule`. **The host application chooses which providers run** — an
`InstanceBuilder` runs none by default, and `enableStandardProviders(True)` adds only the JSON one.

| Provider | How | Enabled by |
| --- | --- | --- |
| JSON | `opendaq-config.json` beside the executable, or at `OPENDAQ_CONFIG_PATH` | `InstanceBuilder().enableStandardProviders(True)` |
| Environment | `OPENDAQ_CONFIG_Modules_BluetoothModule_FilterByKnownProfiles=false` | `.addConfigProvider(EnvConfigProvider())` |
| Command line | `-CModules_BluetoothModule_FilterByKnownProfiles=false` | `.addConfigProvider(CmdLineArgsConfigProvider(args))` |

```json
{ "Modules": { "BluetoothModule": { "FilterByKnownProfiles": false } } }
```

`bt_cli` enables the environment provider, so the variable above works with it directly.

## Bluetooth GATT devices and properties in openDAQ

Device properties `Address`, `Name`, `Connected` and `MtuSize` are read-only mirrors of the
peripheral's state. `Connected` means there is a live link *right now*: it flips to false the
moment one drops and back to true when it is re-established. `MtuSize` is refreshed with it,
because the ATT MTU is renegotiated per link.

The same state is published through openDAQ's standard mechanism, which is what a generic client
watches:

- `device.getStatusContainer().getStatus("ConnectionStatus")`
- `device.getConnectionStatusContainer().getStatus("ConfigurationStatus")`

Both are `ConnectionStatusType` enumerations — `Connected`, `Reconnecting` or `Unrecoverable` —
and a change fires openDAQ's `ConnectionStatusChanged` core event. The accompanying status message
carries the reason a reconnection attempt failed, verbatim from the backend.

Each GATT service becomes a read-only **object property** named after the service, and each
characteristic inside it becomes a property whose value is the characteristic's value. The
property's description carries the UUID and the GATT flags — an object property can hold no other
metadata, and the name is the addressing key.

- **A characteristic without a write bit is read-only**, and setting it throws
  `AccessDeniedException`. A writable one is writable, and setting it performs the GATT write; if
  the peripheral rejects it, the set throws and the property keeps its previous value.
- **Values are typed where the Bluetooth SIG defines the format** — `Battery Level` is an integer
  with a `%` unit, `Manufacturer Name` a string. Everything else, including every vendor
  characteristic, is a hex string such as `DE AD BE EF`.
- **A characteristic that can only notify is still listed**, read-only and empty, so the database
  is a complete mirror. `HeartRate` in the IO folder is what turns those notifications into
  signals.
- **The backend handle is no longer published.** It existed to address a characteristic; a property
  path does that now, and the handle was always an opaque token callers were told not to parse.

### Freshness

Reading a characteristic property returns the value the device already has and schedules a re-read
in the background; the property updates when it lands, firing a change event. A read within a
second of the last one is skipped, so browsing the device does not turn into one radio round trip
per property.

That indirection is not a convenience. openDAQ takes the device's config lock *before* invoking a
property read handler, and a Bluetooth backend thread holds the connection's bus lock while
delivering a notification — so a handler that read from the radio would take those two in the
opposite order and deadlock. The rule the module keeps is that **nothing holding openDAQ's config
lock may block on the radio**.

The practical consequence: the first read after a value changes on the peripheral returns the
previous one, and the next returns the new one. `bt_cli read` prints both when they differ.

A characteristic that has never been read successfully sits at its type's default — `0` for an
integer, empty for a string — and the reason is in the log rather than in the value.

Services and characteristics in the Bluetooth SIG assigned-numbers registry are named
("Battery Service", "Manufacturer Name", "Indoor Bike Data"); vendor-specific UUIDs keep their
identifying prefix. The Fitness Machine data characteristics this module does not decode —
treadmill, rower, cross trainer, step and stair climber — are named too, so a machine that gets no
channels still browses readably enough to show why.

Channels carry properties of their own: read-only mirrors of sensor state, the writable
`WheelCircumferenceMm` on each cycling channel, and the trainer control actions. See
[Supported openDAQ Bluetooth measurement signals](#supported-opendaq-bluetooth-measurement-signals).

## Bluetooth command-line tool

`bt_cli` drives the module exactly as an application would — through an openDAQ `Instance` that
loads the built module from disk.

```bash
./build/bin/bt_cli scan 8
./build/bin/bt_cli connect bluetooth://AA:BB:CC:DD:EE:FF
./build/bin/bt_cli read bluetooth://AA:BB:CC:DD:EE:FF "Battery Service.Battery Level"
./build/bin/bt_cli read bluetooth://AA:BB:CC:DD:EE:FF 2a29        # by name or UUID when unambiguous
./build/bin/bt_cli write bluetooth://AA:BB:CC:DD:EE:FF "Nordic UART.Nordic UART RX" "01 A2"
./build/bin/bt_cli monitor bluetooth://AA:BB:CC:DD:EE:FF 30
./build/bin/bt_cli monitor bluetooth://AA:BB:CC:DD:EE:FF 30 FitnessMachine
```

`monitor` streams every signal of every channel and prints each channel's descriptive properties
first. A smart trainer hosts three profiles at once and around forty signals, which is an
unreadable wall at 4 Hz — pass a channel local id as the last argument to narrow it.

## Test the openDAQ Bluetooth module

```bash
ctest --test-dir build --output-on-failure
```

`bluetooth_module_tests` drives the whole openDAQ layer through a scripted fake BLE backend, so it
needs no adapter and runs identically on every platform. `bluetooth_module_loading_tests` loads the
built shared library through a real openDAQ `Instance` and checks the things unit tests cannot: that
the library is named so the ModuleManager finds it, that its exports resolve, and that the
`bluetooth` prefix routes back to this module.

Both suites can run on Android over `adb` without a radio. `connectedDebugAndroidTest` additionally
checks the Java/JNI binding and catches shrinker removal of the bridge classes.

## Bluetooth platform notes

**Linux.** Needs D-Bus access to `org.bluez` — normally membership of the `bluetooth` group, or a
polkit rule — and a powered adapter. Both conditions are reported as the reason discovery is empty
rather than as a failure.

**Windows.** Requires Windows 10 1709 or later and MSVC. C++17 is sufficient — the backend uses
no coroutines — and C++/WinRT is header-only inside the Windows SDK, so there is nothing to
install: no NuGet package, no `cppwinrt.exe`, no `.winmd` step.

**The host's apartment does not matter.** The backend owns a dedicated MTA thread and runs every
WinRT call on it, so it is safe to call from an STA thread, from a thread that never initialised
COM, and from several threads at once. (This was not true before: it used to initialise an
apartment on whichever thread reached it first, which broke exactly those cases.) Blocking waits
must not run on an STA — C++/WinRT only asserts about it in Debug builds and silently stalls the
thread's message pump in Release — which is why the marshalling is not optional. The corollary is
that WinRT event handlers deliberately run *outside* that apartment, on WinRT's own threadpool
threads: no delegate in the backend may capture a raw `this`.

An unpackaged Win32 desktop application needs **no manifest entry, no capability and no privacy
prompt** to use Bluetooth LE — `<DeviceCapability Name="bluetooth"/>` is an MSIX-only requirement,
and there is no macOS-style authorisation gate. Note that peripherals whose characteristics
require encryption must be paired through Windows Settings first: this module never calls
`DeviceInformation.Pairing.PairAsync`, so such a peripheral surfaces as a non-`Success` GATT status
with a "may require pairing" message.

**macOS.** On macOS 11 and later the *host application bundle* must declare
`NSBluetoothAlwaysUsageDescription` in its `Info.plist`, and the user must allow Bluetooth access
when prompted. A bare unbundled command-line binary is typically denied, often silently — the
backend reports this as an authorisation failure rather than as "no devices".

CoreBluetooth's `retrieveConnectedPeripheralsWithServices:` takes a non-empty service array and has
no "everything" form, so on macOS and iOS a peripheral connected to the host is reported only when
it hosts a service this module has a profile for, even with `FilterByKnownProfiles` off. Linux and
Windows report every connected peripheral and filter afterwards. Reporting connected peripherals at
all is verified on Linux only; the Windows, macOS and iOS paths compile but have not run against a
radio.

**iOS.** Consume the XCFrameworks by adding `ios/` as a local Swift package, or by dragging all
four bundles into the application target's *Frameworks, Libraries, and Embedded Content* with
**Embed & Sign**. All four must be embedded: the application never references the module
symbolically — openDAQ `dlopen`s it — so leaving it out is an easy mistake, and it surfaces only as
an empty device list.

Then, before building the `Instance`:

```objc
// Objective-C++: openDAQ's headers are C++ and cannot be imported into Swift.
NSString* modules = NSBundle.mainBundle.privateFrameworksPath;   // App.app/Frameworks
InstancePtr instance = InstanceBuilder()
    .addModulePath(modules.UTF8String)
    .build();
```

The module path must be explicit for the same reason it must be on Android: openDAQ's default is
the executable's own directory, which on iOS is `App.app` — one level above `Frameworks/`.

The application's `Info.plist` must declare **`NSBluetoothAlwaysUsageDescription`**, and this is
not the advisory requirement it is on macOS: iOS *terminates* an application that touches Bluetooth
without it. Since the module builds its `CBCentralManager` while `ModuleManager` is loading it,
that termination would strike inside `InstanceBuilder().build()` — so on iOS the backend
pre-flights the key and reports an ordinary unavailability reason instead of letting the
application die. `NSBluetoothPeripheralUsageDescription` is an iOS 12-and-earlier key and is not
needed at a 15.0 floor.

**The permission alert therefore appears when the `Instance` is built**, not at the first scan.
Build the instance at a moment the application is ready to explain itself. Unlike macOS, the
backend also passes `CBCentralManagerOptionShowPowerAlertKey: NO`: a module loaded into someone
else's application should not raise a modal "Turn On Bluetooth" alert during instance construction.
`isAvailable()` reports "Bluetooth is turned off" and the application can prompt when it chooses.

`getAvailableDevices()` additionally runs openDAQ's own mDNS device discovery, which has nothing to
do with Bluetooth but is what iOS calls local networking. Without `NSLocalNetworkUsageDescription`,
`NSBonjourServices` and Apple's request-only `com.apple.developer.networking.multicast` entitlement
those queries find nothing. Bluetooth discovery is unaffected.

**Discovery is foreground-only.** The module scans with a `nil` service filter, because that is
what lets one module discover every profile it supports — and iOS delivers nothing from a `nil`
filter while the application is backgrounded, whatever `UIBackgroundModes` says. An *established*
connection and its notifications do survive backgrounding if the application declares the
`bluetooth-central` background mode. `CBCentralManagerScanOptionAllowDuplicatesKey` is likewise
ignored in the background; in the foreground it is a real battery cost, and it is on so that `Rssi`
stays current. Call `stopScan()`, or drop the device, when no scan UI is showing.

**Do not call `getAvailableDevices()` on the main thread.** `isAvailable()` waits up to three
seconds for the manager to reach `CBManagerStatePoweredOn` — on a first launch that window spans
the permission alert — and `ScanCache` blocks its first snapshot for `ScanWarmupMs`, 2000 ms by
default. Five seconds of frozen UI in the worst case. `ScanWarmupMs` is a module option; the
three-second manager wait is not currently configurable.

Peripheral identifiers are per-*device* `NSUUID`s, exactly as on macOS, so a connection string
copied from a Mac is not valid on an iPhone.

**The Simulator has no Bluetooth radio.** `isAvailable()` reports that the device does not support
Bluetooth Low Energy, which is correct rather than a fault. Reaching that message is still a useful
signal: it proves the dylibs embedded, `dlopen` succeeded and the module loaded.

**Android.** Consume the AAR, then, before building the openDAQ `Instance`:

```java
OpenDaqBluetooth.initialize(context);
// ... request OpenDaqBluetooth.requiredPermissions() from an Activity ...
InstancePtr instance = InstanceBuilder()
    .addModulePath(OpenDaqBluetooth.moduleDirectory(context))
    .build();
```

`initialize()` loads the JNI bridge and caches its Java classes. The module path must be explicit
because openDAQ's executable-based default points into `/system/bin` on Android.

The application must also extract native libraries so `ModuleManager` can scan them:

```gradle
android { packaging { jniLibs { useLegacyPackaging = true } } }
```

Request the strings returned by `requiredPermissions()` from an Activity: API 31+ uses
`BLUETOOTH_SCAN` and `BLUETOOTH_CONNECT`; older releases use `ACCESS_FINE_LOCATION`. The AAR ships
consumer ProGuard rules for the JNI bridge.

## Project scope and limitations

Discovery, connection, GATT browsing with characteristic reads and writes, and notification-driven
asynchronous signals for four profiles: Heart Rate, Cycling Power, Cycling Speed and Cadence, and
the Fitness Machine service for indoor bikes — the last including its control point, so a smart
trainer can be driven in ERG, resistance and simulation modes.

The GATT database as a property tree, one object property per service and one property per
characteristic. Automatic reconnection when the link drops, with the device's connection status
published through openDAQ's standard containers.

Not implemented: **profiles beyond those four** (the registry makes them additive); the Fitness
Machine service for treadmills, rowers, cross trainers and stair climbers, which carry their own
data characteristics; the Cycling Power Control Point (0x2A66) and SC Control Point (0x2A55), so
no zero-offset calibration or counter reset; the Cycling Power Vector (0x2A64); Fitness Machine
Status (0x2ADA), so losing control of a trainer to another application shows up only as a
`Control Not Permitted` result on the next request; raw byte signals for arbitrary notifying
characteristics; Bluetooth Classic / RFCOMM; pairing and bonding; peripheral-role operation; and
re-modelling a peripheral whose GATT database changed while connected.

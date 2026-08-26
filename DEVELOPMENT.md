# Development notes

Implementation notes for contributors working on this repository.

## What this is

An openDAQ module that exposes nearby Bluetooth Low Energy peripherals as openDAQ devices.
Discovery lists the peripherals the host adapter can hear that advertise a service the module has a
profile for; `bluetooth://<identifier>` connects to one -- listed or not -- and mirrors its GATT
database into the resulting openDAQ device's property tree.

## Commands

```bash
# Configure (first run fetches openDAQ v3.40.1 and googletest; expect ~15 min of SDK build)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j "$(nproc)"

# Reuse an existing openDAQ checkout instead of cloning again
cmake -S . -B build -G Ninja -DFETCHCONTENT_SOURCE_DIR_OPENDAQ=/path/to/opendaq-src

# Tests
ctest --test-dir build --output-on-failure
./build/bin/bluetooth_module_tests                      # unit tests, no hardware, ~60 ms
./build/bin/bluetooth_module_loading_tests              # loads the built .module.so via an Instance
./build/bin/bluetooth_module_tests --gtest_filter='BluetoothDevice.*'   # a single suite

# Hardware smoke test
./build/bin/bt_cli scan 8                               # exits 1 when the filter hides everything
env OPENDAQ_CONFIG_Modules_BluetoothModule_FilterByKnownProfiles=false ./build/bin/bt_cli scan 8
./build/bin/bt_cli connect bluetooth://AA:BB:CC:DD:EE:FF
./build/bin/bt_cli read bluetooth://AA:BB:CC:DD:EE:FF "Battery Service.Battery Level"
./build/bin/bt_cli read bluetooth://AA:BB:CC:DD:EE:FF 2a29   # by name or UUID when unambiguous
./build/bin/bt_cli write bluetooth://AA:BB:CC:DD:EE:FF "Nordic UART.Nordic UART RX" "01 A2"
./build/bin/bt_cli monitor bluetooth://AA:BB:CC:DD:EE:FF 30                  # every channel's signals
./build/bin/bt_cli monitor bluetooth://AA:BB:CC:DD:EE:FF 30 FitnessMachine   # one channel only
```

Useful CMake options: `OPENDAQ_BT_USE_SYSTEM_SDK` (use an installed SDK rather than FetchContent),
`OPENDAQ_BT_BUILD_TESTS`, `OPENDAQ_BT_BUILD_APP`.

The Android AAR is a separate Gradle project under `android/`; the root remains a CMake project.
There is no committed Gradle wrapper.

```bash
# AAR: android/build/outputs/aar/opendaq-bluetooth-release.aar
cd android && gradle assembleRelease
cd android && gradle connectedDebugAndroidTest   # needs a device or emulator

# Cross-compile tests for adb
cmake -S . -B build-android -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
      -DCMAKE_BUILD_TYPE=Release -DOPENDAQ_BT_BUILD_TESTS=ON
cmake --build build-android -j "$(nproc)"

llvm-strip --strip-unneeded build-android/bin/*.so build-android/bin/bluetooth_module_*tests
adb push build-android/bin /data/local/tmp/odbt
adb shell 'cd /data/local/tmp/odbt && LD_LIBRARY_PATH=. \
           OPENDAQ_BT_MODULE_DIR=/data/local/tmp/odbt ./bluetooth_module_loading_tests'
adb shell 'cd /data/local/tmp/odbt && LD_LIBRARY_PATH=. ./bluetooth_module_tests'
```

`OPENDAQ_BT_MODULE_DIR` overrides the host path compiled into the loading test.

## Signals and profiles

A GATT service the module understands becomes an openDAQ **channel** under the device's IO folder.
The registry lives in `src/profiles/gatt_profile.cpp` — one `GattProfile` entry per service UUID
with a channel factory. Four entries today: Heart Rate (0x180D), Cycling Power (0x1818), Cycling
Speed and Cadence (0x1816) and Fitness Machine (0x1826). Adding another touches nothing outside
that file, its decoder and its channel. The registry drives **discovery** as well as
channels: `advertisesKnownGattProfile` filters the device list against it, so a new profile widens
what `getAvailableDevices()` reports, and `setServicesOfInterest` derives from it too.

Shared scaffolding lives in `profiles/channel_support.h` (domain descriptors, the epoch anchor,
`handleOf`/`characteristicOf`, and the `SignalPair` that keeps one-domain-per-value from becoming
thirty lines of bookkeeping) and `profiles/byte_reader.h` (a little-endian cursor whose reads past
the end set a sticky flag rather than reading out of bounds, so a decoder checks once at the end
instead of guarding every field). `HeartRateChannel` predates both and still carries its own
copies.

- **Signals are asynchronous**: the domain descriptor uses `ExplicitDomainDataRule(min, max)`, so
  each sample carries its own Int64 microsecond timestamp. That is what "async signal" means in
  openDAQ; a `LinearDataRule` would incorrectly imply fixed BLE notification timing.
- **Each value signal has its own domain signal.** Signals sharing a domain must produce the same
  number of samples per packet, and a heart rate notification carries exactly one BPM value but
  zero-or-many RR intervals and an optional energy figure. Do not "simplify" these into one domain.
- **Timestamps are anchored once and advanced by a steady clock** (`epochOffset` + `steady_clock`
  delta in `HeartRateChannel::nowTicks`). Calling `system_clock::now()` per notification would let
  an NTP correction step the domain backwards and break the monotonicity consumers assume.
- **The decoders are pure** — bytes in, struct out, no openDAQ and no BLE types — which is why
  their tests cover the whole flag matrix of each characteristic cheaply. Each returns `nullopt`
  on any payload whose flags promise more than the buffer holds; real peripherals do send short
  frames.
- **Three wire-format traps, each of which produces plausible numbers rather than an error:**
  - **Indoor Bike Data (0x2AD2) bit 0 is inverted.** It is "More Data", and Instantaneous Speed is
    present when it is *clear*. Reading it the intuitive way shifts every subsequent field by two
    octets. Every other bit in that characteristic has ordinary polarity.
  - **Cycling Power's wheel event time ticks at 1/2048 s while its crank event time ticks at
    1/1024 s** — in the same characteristic. CSC (0x2A5B) uses 1/1024 s for *both*. Copying the
    rate across reports exactly twice the real speed.
  - **The FTMS resistance level is asymmetric.** Indoor Bike Data reports it as a whole-numbered
    `sint16`; the control point commands it as a `uint8` in tenths, so it cannot express more than
    25.5. That is the specification, not a mistake. A trainer publishing a larger maximum is
    treating the octet as a whole level and gets a log line saying so.
- **`RevolutionTracker` (`profiles/revolution_tracker.h`) is where cadence and speed come from.**
  Neither cycling service transmits a rate; both transmit a cumulative counter and the timestamp
  of the last revolution. It relies on four guards: modular deltas for the 16-bit
  event time and the 16- or 32-bit counter; `deltaTicks == 0` emits nothing (a stationary sensor
  repeats its last event time, so dividing is a division by zero); a wall-clock gap approaching
  the event-time wrap period re-anchors without emitting; and a **plausibility ceiling on the
  rate**, which is the only guard that catches a sensor that browned out and reconnected inside
  the retry period with its counter reset — modular arithmetic reads that as most of a modulus of
  revolutions in a fraction of a second, and the gap rule cannot see it because barely any time
  passed. The tracker gets no reconnect hook: `ReconnectingConnection` re-applies subscriptions
  silently and its link-state callback is a single slot `BluetoothDevice` already owns, so these
  guards must stay mandatory rather than configurable.
- **A stopped sensor emits one explicit zero after three seconds**, then goes quiet. Without it a
  cadence plot freezes at the last value pedalled, which reads as a dead sensor rather than a
  stopped rider. It is no more synthetic than any other sample here — the sensor never transmits a
  rate at all.
- **Cumulative cycling signals count from the channel's first notification**, not from the
  peripheral's own counter. That counter runs from whenever the battery went in, and the CPS crank
  one is `uint16` — it wraps roughly every four hours of riding, which would put a sawtooth on a
  signal consumers integrate.
- **FTMS control is asynchronous and must stay that way.** A procedure writes its request and
  returns; the trainer's verdict arrives later as an indication and lands in `LastControlResult`.
  Blocking a procedure until the indication arrived would look like a nicer API and would
  deadlock — the procedure holds `sync` while waiting and the indication callback needs `sync` to
  record the result. For the same reason no control-point callback may touch the radio: on Linux
  it runs holding the connection's `bus.mutex`, so auto-re-requesting control after a
  `Control Not Permitted` would deadlock against the thread delivering the answer.
- **`FitnessMachineChannel` sets `shuttingDown` as the first act of its destructor.** Its control
  actions are property values, so an application can hold a `ProcedurePtr` after dropping every
  reference to the channel; calling it then would write through a freed `this`. Every procedure
  and every notification handler takes `sync` and checks the flag.
- **A channel with several subscriptions releases them one at a time.** `FitnessMachineChannel`
  keeps a `subscribedHandles` vector populated at the point of subscription and loops it in the
  destructor with a per-handle `catch (...)`: one `try` around the loop would let a throw on the
  first leave the peripheral notifying into a dead object through the rest. `catch (...)` rather
  than `catch (const std::exception&)` because a `winrt::hresult_error` does not derive from
  `std::exception` and an exception escaping a destructor is `std::terminate()`.
- **Channels are never rebuilt on a reconnect.** `ReconnectingConnection`
  (`reconnecting_connection.h`) wraps the backend connection so the device and its channels hold
  one object for their whole lives; the supervisor swaps what is *inside* it. Rebuilding the
  channels would hand consumers new `SignalPtr`s and silently stop every `StreamReader` attached
  to the old ones — a failure with no error and no exception. `HeartRateChannel` and the profile
  registry needed no changes for this because the backend abstraction contains the reconnect logic.
- **The connection is held by `shared_ptr`, not `unique_ptr`.** A derived destructor body runs
  before its base's, so `~BluetoothDevice` would otherwise release the connection before
  `GenericDevice` tore down the IO folder holding the channels, and `~HeartRateChannel` would
  unsubscribe through a dangling pointer. This previously caused a segfault.
- **Only write a property that actually changed.** `setProtectedPropertyValue` notifies every
  listener synchronously, on the backend thread. A trainer notifying at 4 Hz would otherwise tell
  every listener sixteen times a second about a training status that has not moved all workout.
- **Tests reading signals must skip event packets.** A freshly connected signal emits a
  descriptor-changed event first and a `StreamReader` stops there, so a plain reader returns zero
  samples however much data is queued. Use the `dataReader()` helper in
  `tests/test_heart_rate_channel.cpp` (`StreamReaderBuilder().setSkipEvents(True)`).

## Architecture

The whole design turns on one seam: `bluetooth_module/include/bluetooth_module/ble/ble_central.h`.

```
openDAQ layer     BluetoothModule ─owns─> ScanCache ─drives─> ICentral
(no OS BT headers) BluetoothDevice ─owns─> ReconnectingConnection ─owns─> IConnection
                        │                          └─uses─> ICentral (shared)
─────────── seam: pure virtual, plain C++ structs, no daq:: types ──────────
                        │
BLE backends      ble_central_bluez.cpp          (Linux, sd-bus / org.bluez)
(no openDAQ headers)
                  ble_central_winrt.cpp          (Windows, C++/WinRT)
                  ble_central_corebluetooth.mm   (macOS, Objective-C++)
                  ble_central_android.cpp        (Android, JNI + android/src/main/java shim)
                  tests/fake_ble_central.h       (scripted, used by every unit test)
```

Exactly one backend compiles per platform, selected by `opendaq_bt_add_ble_backend()` in
`cmake/CommonUtils.cmake`. Because nothing above the seam includes an OS Bluetooth header and
nothing below it includes an openDAQ header, the entire openDAQ layer is testable with no radio.

**Linux is built and verified against real hardware. macOS is verified for discovery, connect,
GATT resolution, characteristic reads and heart-rate notification streaming** (arm64/macOS 15,
against a WHOOP 5 strap); **characteristic writes and two peripherals connected at once are still
unexercised there.** **Windows compiles, links and loads (ARM64/MSVC) and its discovery path is
exercised, but it has never seen a radio** — the bring-up host had no Bluetooth adapter — **and
x64 has never been compiled.** Do not describe Windows scanning, connecting, GATT browsing or
heart-rate streaming as working.

**Android cross-compiles and loads (arm64-v8a, minSdk 26), but has not been tested with a radio.**
The loading and unit suites pass on an emulator; all radio and permission paths remain unverified.

The CoreBluetooth backend keeps **one `PeripheralState` block per connected peripheral**, held by
`shared_ptr` and registered in `SharedState::peripherals` under the peripheral's identifier; every
delegate callback routes through `stateOf(state, peripheral)`. This prevents a bug caused by maps
keyed by characteristic handle, which on this backend is
`serviceUuid + "/" + characteristicUuid` and so is only unique *within* a peripheral. Two heart
rate straps both key their measurement characteristic identically, so a single shared block meant
the second `subscribe()` overwrote the first, both peripherals' notifications went to whichever
callback registered last, one device's teardown cleared everyone's callbacks, and a single
`connected` flag and `disconnectCallback` were shared by every device. The connect handshake lives
in that block for the same reason — connecting to a second peripheral must not disturb one still
resolving its GATT database. `~CoreBluetoothConnection` clears its callbacks *before* removing the
block from the table, and removes it only if the table still points at its own block, since
connecting twice to one peripheral replaces the entry.

The Windows backend owns a **dedicated MTA apartment thread** and marshals every WinRT call, event
registration/revocation and WinRT object release onto it, so it is independent of whatever
apartment the host put its threads in. Blocking waits on an `IAsyncOperation`
must not run on an STA thread, and C++/WinRT only asserts about it in Debug (in Release it silently
stalls the message pump). Two rules follow, both easy to undo by accident:

- WinRT event handlers deliberately run *outside* that apartment, on WinRT's own threadpool, so
  **no delegate may capture a raw `this`** — handler state lives in `shared_ptr` blocks
  (`ConnectionState`, `ScanState`), exactly as the CoreBluetooth backend does with `SharedState`.
- `winrt::hresult_error` does **not** derive from `std::exception`, so it is translated to
  `BleError` at the single `Apartment::run()` choke point. Without that, a WinRT failure sails past
  every `catch (const std::exception&)` above the backend — including the one in
  `HeartRateChannel`'s destructor, where it means `std::terminate()`.

Android needs a Java shim because Bluetooth is not exposed by the NDK. `OpenDaqBluetooth` is the
entry point, while `ScanBridge` and `GattBridge` forward platform calls and callbacks. Policy and
timeouts stay in `ble_central_android.cpp`. Callbacks use integer tokens backed by weak registries,
so late callbacks cannot dereference destroyed C++ objects.

`enable_language(OBJCXX)` for the macOS backend lives in the **top-level** `CMakeLists.txt`, not
in `opendaq_bt_add_ble_backend()`. CMake only accepts `enable_language()` at file scope: called
from a function it loads the language's rule variables into that function's scope, where they are
discarded on return. Only the `CMAKE_OBJCXX_COMPILER` cache entry survives, so configuration
succeeds and `flags.make` even lists an OBJCXX compiler -- but `OBJCXX_INCLUDES` loses its `-I`
prefixes, `OBJCXX_FLAGS` loses `-std=` and `-fPIC`, and the generated `.mm` compile rule has no
compile command at all. The build then fails at link time on a missing `.mm.o`. `CMAKE_OBJCXX_*`
dialect settings are also separate from `CMAKE_CXX_*` and are set alongside it.

### Things that are non-obvious

- **A connection must never be destroyed from inside its own disconnect callback.** `~BluezConnection`
  calls `bus.close()` → `stopDispatch()` → `dispatcher.join()`, and the callback is delivered *on*
  that dispatch thread — so tearing down there joins the thread to itself. That is why
  `ReconnectingConnection` has a supervisor thread at all: the callback only sets a flag and
  notifies. `FakeConnection`'s destructor asserts on this, because a fake has no dispatch thread
  and the mistake would otherwise only show up on hardware.
- **A BlueZ connection disconnects the peripheral as it is destroyed**, on a device object path
  derived from the MAC — the same path a reconnected link uses. So the dead connection must be
  fully released *before* `connect()` is called again, or its destructor tears the new link
  straight back down. `Reconnect.ReleasesTheOldConnectionBeforeConnectingAgain` guards the order.
- **Every BlueZ callback runs holding that connection's `bus.mutex`** — `Bus::dispatchLoop` calls
  `sd_bus_process` inside `std::scoped_lock guard(mutex)`. So the disconnect callback flips the
  `Connected` property and nothing else; statuses and core events go out from the supervisor
  thread instead. Publishing a status there would let a listener that reads a characteristic
  deadlock against the very thread delivering the notification.
- **`services()` is frozen at the first resolved database and never replaced.** `IConnection` hands
  back a reference, the channels hold a `Service&` from it, and every property
  name in the device is derived from it once. `ReconnectingConnection` translates each frozen handle
  to the live one through a remap keyed on (service UUID, service ordinal, characteristic UUID,
  characteristic ordinal), so a peripheral that comes back with renumbered handles keeps the same
  property tree. A service that appears after a reconnect is invisible — the documented price of
  never invalidating a reader or moving a name under a user.
- **A link state handler must upgrade a `WeakRefPtr`, not capture a raw `this`.** openDAQ frees a
  component's weak-ref bookkeeping the moment its strong count reaches zero — inside `releaseRef`,
  *before* the destructor body that stops the reconnector runs. A supervisor thread calling
  `setProtectedPropertyValue` on a device whose last reference was just dropped therefore
  segfaults inside `getWeakRef`. Upgrading a weak reference either fails, or holds the device
  alive for the callback. It must stay weak: a strong capture is the self-owning cycle
  `LeavesNoDeviceBehind` exists to catch.
- **`ReconnectingConnection` keeps its state in a `shared_ptr` block the supervisor captures.**
  Because a handler holds a device reference, releasing it can destroy the device, its channels and
  the connection itself *on the supervisor's own thread*; without the indirection the supervisor
  would return into freed members. `shutdown()` detaches rather than joins in that case. Same
  pattern, same reason, as the WinRT and CoreBluetooth event handlers.
- **Stop the reconnector from `removedNoLock()`, not `removed()`.** `ComponentImpl::remove()` calls
  `removed()` while holding the recursive config lock and `removedNoLock()` after releasing it, and
  the supervisor takes that same lock through `setProtectedPropertyValue`. Joining it from
  `removed()` deadlocks. For the same reason `~BluetoothDevice` calls `shutdown()` *before* taking
  `sync`.
- **The reconnector starts a scan before each attempt and never stops one.** BlueZ evicts a
  non-bonded peripheral from its object tree once it stops being heard, after which `connect()`
  fails with "is not known to BlueZ" forever — retrying without scanning is a guarantee of never
  succeeding. Stopping is the destructive half: the scan is a process-wide resource `ScanCache`
  owns.

- **Discovery hides peripherals with no matching profile, by default.** `onGetAvailableDevices`
  drops any advertisement whose `serviceUuids` contain nothing `gattProfiles()` claims — *and* any
  advertisement with an empty list, which is strict on purpose. So `getAvailableDevices()` reports
  far fewer devices than `bluetoothctl` does (1 vs 88 on a real desk). The filter lives in
  `BluetoothModule`, **not** in `ScanCache`, because `onCreateDevice` uses `snapshot()` to recover
  the advertised display name and filtering in the cache would silently unname any peripheral
  connected to by string; `KeepsTheAdvertisedNameOfAPeripheralDiscoveryFilteredOut` guards that.
  Turn it off with the `FilterByKnownProfiles` module option.
- **Module options are the only module-level config channel that works in production**, and they
  deliberately override the constructor's config object rather than the other way round.
  `DEFINE_MODULE_EXPORTS` gives the ModuleManager only `createModule(IModule**, IContext*)`, so the
  config-taking constructor is a test seam and its fallback is `createDefaultConfig()` —
  indistinguishable from a value a caller set deliberately. If the config won, the production key
  would be dead on arrival. `ScanWarmupMs` and `DeviceTimeoutMs` in an `addDevice` config are read
  but never used for the same reason; only `ConnectionTimeoutMs` is live there.
- **openDAQ runs no config provider by default.** `InstanceBuilder`'s `useStandardProviders` is
  false, and enabling it adds the JSON provider alone; `EnvConfigProvider()` and
  `CmdLineArgsConfigProvider()` must be added by the application, which is why `bt_cli` adds the
  environment one. The env and command-line providers split keys on `_`, so a module option key
  must never contain an underscore — `FilterByKnownProfiles` is safe, `Filter_By_Known_Profiles`
  would silently explode into nested dictionaries. A value arriving as a string rather than a bool
  is ignored, with a warning, rather than coerced.
- **openDAQ's log macros carry their own trailing semicolon.** A brace-less `if (x) LOG_D(...)`
  swallows the statement after it — which compiles, and in `onGetAvailableDevices` silently ate the
  `return`. Always brace a log-only branch.
- **`ScanCache` exists because of a timing mismatch.** `onGetAvailableDevices()` is synchronous and
  must return promptly, but a BLE scan takes seconds. The scan starts lazily and runs continuously;
  each call returns an immediate snapshot minus stale entries. Only the first call blocks, for a
  bounded warm-up (`ScanWarmupMs`, default 2000). Tests pass 0 — otherwise every test costs 2 s.
- **Notifications on Linux are a `PropertiesChanged` carrying `Value`** on the characteristic's own
  object path, after `GattCharacteristic1.StartNotify()`. `sd_bus_match_signal` matches paths
  exactly and characteristic paths are known at subscribe time, so there is one exact match per
  subscription rather than path-namespace filtering.
- **On macOS, `didUpdateValueForCharacteristic:` serves reads *and* notifications** with nothing to
  distinguish them; the backend attributes an update to an outstanding read if there is one, and
  treats it as a notification otherwise. Do not issue a read and a subscription to the same
  characteristic concurrently. The refresh worker makes that easier to trip over than it used to
  be: it re-reads every *readable* characteristic whenever one is browsed. Nothing collides today
  because the only subscribed characteristic, Heart Rate Measurement, is notify-only and so is
  never refreshed -- but a profile that subscribes to a read-and-notify characteristic would break
  this on macOS.
- **Discovery counts only genuine RSSI updates as sightings (Linux).** BlueZ's object tree is a
  persistent cache, and it re-announces every device it has met via `InterfacesAdded` the moment
  discovery starts — which reported over 160 devices where about 80 were in range. So
  `onInterfacesAdded` deliberately does nothing, and only a `PropertiesChanged` carrying `RSSI`
  stamps a device as seen. The discovery filter sets `DuplicateData` so those arrive per
  advertisement. Cross-check with `bluetoothctl --timeout 8 scan le`; we should report fewer.
  That is half the rule; the connected-peripheral bullet below is the other half.
- **A peripheral connected to this host is discoverable even though it is not advertising.** BlueZ
  drops a device's `RSSI` property the moment it connects, so it can never be *heard* again -- and
  it is the one peripheral the module is certain it can reach. `getDiscovered()` therefore has a
  second inclusion path: `Connected && ServicesResolved`. `ServicesResolved` is not decoration.
  That loop walks the whole object tree, which is *not* restricted to LE the way the scan filter
  is, so `Connected` alone would start reporting Bluetooth Classic headsets; only an LE link
  resolves a GATT database. Such an entry carries `Advertisement::connected`, an `rssi` of 0
  meaning *unknown*, and a `lastSeen` stamped at query time -- and `ScanCache::snapshot()` exempts
  it from `DeviceTimeoutMs` pruning, because a sighting that predates the link only ever gets
  older. It cannot get stranded: the backend reports it only while the link is up, so it leaves on
  the first snapshot afterwards. This is what made `bt_cli scan` show nothing on a desk with a
  heart rate strap on it -- a killed `monitor` had left the link open, and BlueZ keeps it.
- **`Device1.Connect` is skipped when BlueZ already reports the device connected.** The
  `GetManagedObjects` reply `connect()` already fetches answers the question for free, and whether
  `Connect` on an established link is a no-op or an `AlreadyConnected` error is not the same across
  BlueZ versions. Both are tolerated; neither is relied on.
- **`ConnectionTimeoutMs` is a per-phase budget**, applied separately to establishing the link and
  to resolving the GATT database. Sharing one budget across both made connections fail on a busy
  adapter when a slow connect starved service discovery.
- **The `bluetooth` prefix is the entire routing mechanism.** `DeviceType(id, name, description,
  prefix, config)` takes the prefix verbatim as the part before `://`. It is defined once, as
  `opendaq_bt::ConnectionStringPrefix` in `connection_string.h`.
- **Connection strings are host-specific by design.** Linux/Windows use the MAC; macOS
  CoreBluetooth never exposes the MAC and yields a per-host `NSUUID` instead. A string copied
  between machines will not resolve. The parser accepts both shapes and normalises them.
- **Peripheral state is exposed as genuinely read-only properties**, updated internally through
  `IPropertyObjectProtected::setProtectedPropertyValue`. An earlier version used a write handler
  that reverted writes, which also silently reverted the module's own updates — two tests exist to
  keep that regression from returning.
- **The GATT database is the property tree.** One read-only `ObjectProperty` per service, built by
  `initGattProperties()`, and inside it one property per characteristic whose *value is the
  characteristic's value* -- read-only unless GATT reports a write bit, in which case a set
  performs the write. There is no `GattDatabase` list, no struct types and no
  `ReadCharacteristic`/`WriteCharacteristic`: a property path is the addressing key, so
  `resolveCharacteristic`'s "refuse an ambiguous UUID rather than guess" problem does not arise
  inside the module at all. It survives only in `bt_cli`, where a user still types a short name.
- **Property names are the public API, and they are derived once.** `propertyNameFor` takes
  `displayLabel(uuid)`, strips `.`, `[` and `]` -- openDAQ splits a property path on `.` and
  truncates a name at `[` -- and appends " 2", " 3" for anything already taken in that namespace.
  It is called once, over `ReconnectingConnection::services()`, which is frozen; that, and nothing
  else, is why names survive a reconnect. Adding a UUID to `gatt_names.cpp`'s table renames a
  property, so treat that table as an interface.
- **Nothing holding openDAQ's config lock may block on the radio.** `getPropertyValue` and
  `setPropertyValue` take that lock *before* invoking a read or write handler
  (`property_object_impl.h:1649`, `:913`). A Bluetooth backend thread holds the connection's bus
  mutex -- BlueZ calls `sd_bus_process` under it -- so a handler that touched the radio would take
  the two in the opposite order. Hence: read handlers only queue a refresh for the worker thread,
  and *no backend callback may set an openDAQ property*. That is why `BluetoothDevice` hooks
  `setLinkStateCallback` (delivered on the reconnector's own thread) and deliberately leaves
  `setDisconnectCallback` unused.
- **`setProtectedPropertyValue` fires write handlers.** So the refresh worker writing back a value
  it just read from a characteristic that is both readable and writable would push it straight to
  the peripheral, for ever. `applyValue` stamps `suppressWriteHandlerFor` with its thread id and
  every write handler checks it. `DoesNotWriteBackAValueItJustRefreshed` guards this.
- **An event handler's closure must be trivially relocatable -- never capture a `std::string`.**
  openDAQ stores a handler in a fixed inline buffer and *bitwise* moves it (`delegate.hpp`,
  `spec::inplace`'s move constructor memcpys the storage). A string short enough for the
  small-string optimisation keeps a pointer into its own body, so the copy's pointer aims at the
  buffer it came from and the handler reads freed memory. `attachGattHandlers` captures a
  `const CharacteristicBinding*` into the `bindings` map, whose value addresses are stable.
- **`ObjectProperty` keeps the object you hand it, and freezes a clone as the default**
  (`property_object_impl.h:2329-2340`). So the `PropertyObjectPtr` kept in `CharacteristicBinding`
  is the live one to register handlers on, not a copy. Leave the service objects on the default
  `LockingStrategy::OwnLock`: `InheritLock` would merge their mutexes into the device's.
- **A property handler must not capture a pointer that owns it.** Every characteristic handler
  lives on the device it acts on, so capturing a `DevicePtr` would make the device hold a reference
  to itself — a refcount cycle no destructor can break. They capture a raw `this`.
  `LeavesNoDeviceBehind` guards this with a `WeakRefPtr`, and *waits*: a refresh in flight
  legitimately holds a reference for as long as it runs.
- **The refresh worker keeps its state in a `shared_ptr` block, like the reconnector.** A refresh
  upgrades a `WeakRefPtr` to the device, so releasing it can be what destroys the device — on the
  worker's own thread. `stopRefreshWorker()` therefore detaches rather than joins in that case, and
  the loop touches only the state block once the reference is released. `BluetoothDevice` has no
  `sync` mutex any more: the tree holds the values, and `bindings` is written once at construction.
- **The unit-test binary deliberately does not link a real backend.** `tests/test_app.cpp` defines
  `createPlatformCentral()` to throw, so any accidental use of the production path fails loudly
  instead of touching a radio. `bluetooth_module_loading_tests` uses a separate main for this
  reason.
- Serialize Android GATT operations per connection.
- Keep `JNI_OnLoad` optional: `ModuleManager` uses `dlopen`, so initialization must also capture the
  VM and export normal `Java_...` symbols.
- Cache application classes during `OpenDaqBluetooth.initialize()`; attached native threads cannot
  resolve them with `FindClass`.
- An uninitialized bridge must report unavailable, not prevent the module from loading.
- Notifications require both local enablement and a CCCD write.
- Keep both pre-33 and API 33 read/notification callback overloads.
- Keep scans idempotent to avoid Android's scan-start quota.
- Close every `BluetoothGatt`, including failed handshakes and delayed cleanup after permission loss.
- Handles include service and characteristic instance IDs.

### Build constraints imposed by the SDK

The following settings are required for a successful build.

- `CMAKE_CXX_EXTENSIONS` must stay **ON**. openDAQ's object-factory macros rely on the GNU
  `,##__VA_ARGS__` comma-swallowing extension and do not compile under strict `-std=c++17`.
- `OPENDAQ_RELEASE_WARNINGS_AS_ERRORS`/`_DEBUG_` are forced OFF for the fetched SDK. openDAQ builds
  its own sources with `-Werror`, and newer toolchains than it was released against are fatal
  otherwise (GCC 15 / libstdc++ 15 deprecates `std::wstring_convert`, which openDAQ still uses).
- **GoogleTest is fetched in the top-level `CMakeLists.txt`, before openDAQ.** The SDK's CMake
  helpers replace the global `include_directories()` with one that hard-errors under their
  "MODERN" mode, and GoogleTest still calls it.
- `OPENDAQ_MODULE_SUFFIX` is only defined by openDAQ's *installed* package config, so
  `bluetooth_module/CMakeLists.txt` derives it when consuming the SDK via FetchContent. The module
  is invisible to the ModuleManager without the `.module.so`/`.dll`/`.dylib` suffix.
- `src/module_dll.cpp` alone is compiled with `OPENDAQ_TRACK_SHARED_LIB_OBJECT_COUNT`, because
  `DEFINE_MODULE_EXPORTS` defines `daq::daqSharedLibObjectCount` whose declaration sits behind
  that guard.
- Preserve AGP's `CMAKE_LIBRARY_OUTPUT_DIRECTORY`; it is where AAR packaging looks for `.so` files.
- Tests and the CLI default to off on Android but remain switchable.
- openDAQ v3.40.1 cross-compiles unpatched. minSdk 26 also satisfies mDNS's `getifaddrs` requirement.

### Windows on ARM64

openDAQ v3.40.1 does not build on Windows/ARM64 as it stands. Two of its vendored dependencies
pick x86 code paths and fail; both come from the same root cause, and the top-level
`CMakeLists.txt` fixes them together. The symptoms are worth recognising, because neither error
message mentions the architecture that actually went wrong:

- `emmintrin.h(20,1): error C1189: This header is specific to X86, X64, ARM64, and ARM64EC
  targets` while compiling `daqcoretypes`. RapidJSON turned on `RAPIDJSON_SSE2`. The message is
  misleading -- MSVC's `emmintrin.h` only accepts an ARM64 target when `USE_SOFT_INTRINSICS` is
  defined, which nothing here does.
- `masm.targets(70,5): error MSB3721: The command "echo MASM not supported on this platform"`
  while building `boost_context`. Boost.Context selected the `x86_64`/`ms`/`masm` combination and
  handed x86 assembly to an assembler that does not exist for this host.

**Root cause.** `CMAKE_SYSTEM_PROCESSOR` names the *host*, and CMake takes it from the environment
of its own process. Every cmake.exe distributed for Windows is still an x64 binary, so on an ARM64
machine it runs emulated and reports `AMD64` -- from a native ARM64 PowerShell just as much as from
an emulated shell, since it is the emulation and not the shell that decides. Meanwhile the
toolchain being driven is genuinely ARM64 (`CMAKE_CXX_COMPILER_ARCHITECTURE_ID`, and the
`cl.exe`/`link.exe` under `Hostarm64`). Every dependency that branches on `CMAKE_SYSTEM_PROCESSOR`
therefore configures itself for x86. openDAQ's own `BUILD_ARM` never fires either: it is guarded on
`UNIX AND CMAKE_SYSTEM_PROCESSOR MATCHES aarch`, so the SDK has no Windows/ARM64 path at all.

Setting `CMAKE_SYSTEM_PROCESSOR` to `ARM64` after `project()` corrects every consumer at once --
RapidJSON drops to its scalar path, and Boost.Context resolves to `arm64`/`aapcs`/`pe`/`armasm`,
which is exactly the combination its ARM64 `.asm` sources are written for. `BUILD_ARM` is
deliberately *not* set: it would additionally hand `-Wno-error=type-limits`, a GCC flag, to MSVC.

**`cmake/CMakeASM_ARMASMLinkerInformation.cmake`** exists because of a second, unrelated collision.
CMake has no `ASM_ARMASM` language -- its Microsoft ARM assembler dialect is `ASM_MARMASM` -- so
Boost 1.82 ships its own language definition for `ASM_ARMASM` in `libs/context/cmake`. CMake 3.31
then added a mandatory fourth per-language module, `CMake<LANG>LinkerInformation.cmake`, which
Boost 1.82 predates, and configuration aborts with `Could not find cmake module file`. That file
supplies it; it is found because the top-level `CMakeLists.txt` puts `cmake/` on
`CMAKE_MODULE_PATH`. It can go once openDAQ pins a Boost that knows about the CMake 3.31
requirement. The `CMAKE_SYSTEM_PROCESSOR` correction has a different expiry: it becomes a no-op
the day a native ARM64 cmake.exe is what people run, and is harmless in the meantime.

Verified on Windows 11 ARM64 with MSVC 14.44 and CMake 3.31.3, using the Visual Studio 17 2022
generator (Ninja was not installed on that machine, so the `-G Ninja` line above is untested
there; the fix keys off `CMAKE_CXX_COMPILER_ARCHITECTURE_ID`, which MSVC reports under either
generator). `dumpbin /headers` reports `AA64 machine (ARM64)` for `bluetooth_module.module.dll`.
Note that the Boost static libraries are still named `...-x64-1_82.lib`: that tag is Boost's own
address-model naming and says nothing about the ISA -- those archives contain ARM64 objects.

### openDAQ API notes worth not rediscovering

- Module base: subclass `daq::Module`, override `onGetAvailableDevices`, `onGetAvailableDeviceTypes`,
  `onCreateDevice`. Export with `DEFINE_MODULE_EXPORTS(BluetoothModule)`.
- Device base is `GenericDevice<>` (alias `daq::Device`), ctor `(ctx, parent, localId, className, name)`.
- `DeviceInfo(...)` returns a `DeviceInfoConfigPtr` that openDAQ **freezes** after discovery, so
  every custom property must be added before returning it.
- Child components attach via the protected `addFolder(localId, parent)` / `addComponent(localId,
  parent)` helpers on `SignalContainerImpl`.
- openDAQ consumes its thread-local error info when constructing an exception, so
  `daqGetErrorInfoList` is empty by the time a caller catches. Throw openDAQ exception types
  (`DAQ_THROW_EXCEPTION`) with a useful message, and log the real cause in the module — that log
  line is what users actually see.

## Scope

Discovery, connect, the GATT database as a property tree with typed values and characteristic
read/write, asynchronous signals for the Heart Rate, Cycling Power, Cycling Speed and Cadence and
Fitness Machine profiles — the last including its control point, so a smart trainer can be driven
in ERG, resistance and simulation modes — and automatic reconnection with the connection status
published through openDAQ's status containers.

**Only the heart rate profile has been run against real hardware.** The three cycling profiles are
covered by unit tests over the full flag matrix of each wire format and share all the connection
and signal machinery heart rate exercises, but no trainer, power meter or cadence sensor has been
on the other end, and trainer control depends on the characteristic-write path documented above as
unexercised on macOS. They should not be described as verified.

Android ships an AAR, not an application. The consuming app requests runtime permissions.

Deliberately **not** implemented: profiles beyond those four; the Fitness Machine service for
treadmills, rowers, cross trainers and stair climbers, whose data characteristics are named in
`gatt_names.cpp` but not decoded (`FitnessMachineChannel` declines a machine with no 0x2AD2, which
is how they are politely skipped); the Cycling Power Control Point (0x2A66) and SC Control Point
(0x2A55), so no zero-offset calibration or counter reset; the Cycling Power Vector (0x2A64);
Fitness Machine Status (0x2ADA), so losing control of a trainer to another application surfaces
only as a `Control Not Permitted` result on the next request; raw byte signals for arbitrary
notifying characteristics; Bluetooth Classic / RFCOMM; pairing and bonding; peripheral-role
operation; and re-modelling a peripheral whose GATT database changed while connected.

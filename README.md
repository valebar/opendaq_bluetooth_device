# openDAQ Bluetooth device module

An [openDAQ](https://opendaq.com) module that makes nearby **Bluetooth Low Energy** peripherals
appear in openDAQ's normal device discovery and lets an application connect to one with a
`bluetooth://…` connection string.

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

## Measurement signals

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

## Status

| Platform | Backend | State |
| --- | --- | --- |
| Linux | BlueZ over D-Bus (sd-bus) | **Built and verified against real hardware** |
| Windows | C++/WinRT | **Compiles and loads (ARM64, MSVC)** — not yet run against a radio, see below |
| macOS | CoreBluetooth (Objective-C++) | **Built and verified against real hardware** (arm64, macOS 15) |

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

## Building

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

## Connection strings

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

## Losing the link

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

## Configuration

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

## What a connected device exposes

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
[Measurement signals](#measurement-signals).

## Command line

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

## Tests

```bash
ctest --test-dir build --output-on-failure
```

`bluetooth_module_tests` drives the whole openDAQ layer through a scripted fake BLE backend, so it
needs no adapter and runs identically on every platform. `bluetooth_module_loading_tests` loads the
built shared library through a real openDAQ `Instance` and checks the things unit tests cannot: that
the library is named so the ModuleManager finds it, that its exports resolve, and that the
`bluetooth` prefix routes back to this module.

## Platform notes

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
no "everything" form, so on macOS a peripheral connected to the host is reported only when it hosts
a service this module has a profile for, even with `FilterByKnownProfiles` off. Linux and Windows
report every connected peripheral and filter afterwards. Reporting connected peripherals at all is
verified on Linux only; the Windows and macOS paths compile but have not run against a radio.

## Scope

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

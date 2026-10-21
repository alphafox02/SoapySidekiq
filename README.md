# SoapySidekiq

SoapySDR support module for Epiq Solutions Sidekiq SDR devices.

See [CONTRIBUTING.md](CONTRIBUTING.md) for the C++17 coding baseline, formatter,
static analysis, compiler warnings, and CI/non-hardware test instructions.

## Minimum dependencies

- CMake 3.21 or newer (including standalone tests and preset commands)
- A C++17-capable compiler
- SoapySDR development files, version 0.4.0 or newer
- Sidekiq SDK version 4.26.0 or newer

## Sidekiq SDK discovery

The build locates the Sidekiq SDK in this order:

1. `-DSIDEKIQ_SDK_DIR=/path/to/sdk`
2. `SIDEKIQ_SDK_DIR` from the environment
3. `$HOME/sidekiq_sdk_current`

The selected SDK must provide `bin/sidekiq-config` and be version 4.26.0 or
newer. The installer does not bundle an SDK.

## Build

Configure and build with CMake:

```bash
cmake -S . -B build
cmake --build build
```

The build respects `CXX`, `-DCMAKE_CXX_COMPILER=...`, and CMake toolchain files.
The module requests C++17 without compiler extensions. For example:

```bash
cmake -S . -B build/clang -DCMAKE_CXX_COMPILER=clang++-14
cmake --build build/clang --parallel 2
```

The `Sidekiq::sidekiq` imported target carries the SDK include paths, compiler
options, and ordered static-link flags. Changing `SIDEKIQ_SDK_DIR` in an existing
build directory reselects `sidekiq-config` without clearing its cache manually.
For cross-compilation, supply your toolchain file and a compatible SDK build
configuration through the SDK's `SIDEKIQ_CONFIG_BUILD_CONFIG` environment
variable. The ARM compiler/sysroot and SDK binaries must match; a host build
does not validate that combination. See the tested [AArch64 cross-build
workflow](docs/AARCH64_BUILD.md) and optional GNU toolchain file.

### Presets

List presets with `cmake --list-presets`. They respect the chosen generator and
compiler and use separate build directories:

| Preset | Purpose | SDK required |
| --- | --- | --- |
| `dev` | Debug module; install prefix `build/dev-install` | Yes |
| `non-hardware` | Portable regression suite | No |
| `non-hardware-asan` | Portable suite with ASan/UBSan | No |
| `non-hardware-tsan` | Portable suite with TSan on a compatible host | No |
| `non-hardware-coverage` | Portable suite with GCC/Clang coverage | No |

```bash
cmake --preset dev
cmake --build --preset dev --parallel 2
cmake --install build/dev
SOAPY_SDR_ROOT="$PWD/build/dev-install" SoapySDRUtil --check=sidekiq

cmake --preset non-hardware
cmake --build --preset non-hardware --parallel 2
ctest --preset non-hardware
tests/non_hardware/run_suite.sh build/non-hardware
```

Presets export `compile_commands.json` in their build directories. Keep local
overrides in ignored `CMakeUserPresets.json`. The schema uses
[CMake 3.21 presets](https://cmake.org/cmake/help/v3.21/manual/cmake-presets.7.html).
Instrumentation applies only to portable tests. ASan/UBSan, TSan, and coverage
use separate presets/directories; do not enable them together.

Without presets, `SOAPYSIDEKIQ_BUILD_MODULE` defaults to `ON`, and
`SOAPYSIDEKIQ_BUILD_TESTS` defaults to `OFF`. Set the former to `OFF` and the
latter to `ON` for SDK-free root builds. The standalone
`cmake -S tests/non_hardware -B ...` interface remains supported too.

### Installation and removal

Install the module with:

```bash
sudo cmake --install build
sudo ldconfig
```

An explicit `-DCMAKE_INSTALL_PREFIX=...` wins; otherwise the default follows
SoapySDR's prefix when SoapySDR is outside `/usr`. Install over an existing
prefix with `cmake --install` again. Use a separate build directory when changing
compilers or toolchains.

The module build provides an `uninstall` target that removes files in its latest
`install_manifest.txt`, leaves directories and unrelated files intact, and
accepts the same `DESTDIR` used for a staged installation:

```bash
cmake --build build/dev --target uninstall
```

Use elevated permissions for a system-prefix install/uninstall. Installing with
`--prefix` updates the manifest for that installation. Keep the manifest until
removal; the target fails if no install has been recorded.

The `install.sh` helper builds in `build/install-driver` without deleting other
build directories. Its optional argument is a CMake variable such as
`SIDEKIQ_SDK_DIR=/path/to/sdk`. Set `SOAPYSIDEKIQ_INSTALL_PREFIX` to install
without `sudo` into an isolated location:

```bash
SOAPYSIDEKIQ_INSTALL_PREFIX="$PWD/build/local-install" \
  ./install.sh SIDEKIQ_SDK_DIR=/path/to/sdk
SOAPY_SDR_ROOT="$PWD/build/local-install" SoapySDRUtil --info
```

An explicit prefix also works as `./install.sh CMAKE_INSTALL_PREFIX=/path/to/install`
(or `CMAKE_INSTALL_PREFIX:PATH=/path/to/install`). This argument overrides the
environment prefix. Both forms install without `sudo`; the chosen directory
must be writable by the caller. Without either prefix, the helper clears a
previous isolated prefix, installs into CMake's default system prefix with
`sudo`, and runs `ldconfig`. Set `SOAPYSIDEKIQ_BUILD_DIR` to use a different
build directory.

`remake.sh` forwards to `install.sh`; it no longer requires Bear, cleans a build
tree, or copies a compile database into the repository root. Use the database
generated in the selected build directory directly.

## Streaming limitations

The current streaming implementation supports:

- at most one active RX stream per device
- at most one active TX stream per device
- one requested channel per stream

Simultaneous RX and TX are supported. Multi-channel Soapy streams are not currently implemented.

TX accepts CS16 or normalized CF32 and requires each nonempty `writeStream`
request to contain a whole number of `getStreamMTU()` samples, with at most
`INT_MAX` samples per call. It does not stage partial hardware blocks. Invalid
counts or null nonempty buffers return `SOAPY_SDR_STREAM_ERROR`; a zero-length
write returns zero without reading buffers. If back-pressure accepts only a
prefix, advance the input by that many complex samples and retry the suffix.
CF32 components clip to [-1, 1], infinities clip to the corresponding endpoint,
and NaN converts to zero before scaling to the card's TX full scale.

`listSampleRates()` retains a compatibility list spaced at 250 kHz within the
SDK's reported bounds. It does not enumerate the exact profiles of cards with
discrete rate support; the SDK validates each requested rate.

Antenna APIs expose the currently configured RF port. Selecting that same
port is supported; switching to another port is not yet implemented. Cards
without RF-port reporting return no antenna names.

RX automatic DC-offset correction selects the DC calibration type and enables
the SDK's shared automatic calibration mode. Other selected calibration types
are preserved and can also run automatically in that mode. Disabling DC-offset
correction clears only its selection bit and preserves the shared mode.
The SDK can run selected calibrations during subsequent RX tuning operations.

Automatic gain control is exposed for RX. TX uses manual attenuation and
reports `hasGainMode()` as false; requesting TX automatic mode raises an error.

TX exposes two views of the same attenuation control. The existing aggregate
gain and named `LNA` element keep gain-style semantics: larger values reduce
attenuation. The named `attenuation` element uses the SDK's direct dB semantics:
larger values increase attenuation. Its range follows the selected handle's
SDK limits in 0.25 dB steps; use `setGain(SOAPY_SDR_TX, channel, "attenuation", dB)`
and the matching named `getGain()`/`getGainRange()` calls. These are aliases for
one control, not independent gain stages.

`setHardwareTime` accepts nonnegative nanoseconds with an empty/`now` selector
for an immediate update or `1pps` for the next PPS. Conversion uses the system
clock frequency and truncates fractional ticks with checked integer arithmetic.
The SDK immediate update sets both RF and system counters to the same raw tick
value; the PPS update sets RF to zero and system to the requested tick value.
The counters can therefore represent different times when their rates differ.

PPS source selection rejects unknown names. If the SDK source API fails, the
existing kernel PPS endpoint is used when available. Selecting external or
internal PPS requires a timestamp transition across one one-second observation
interval; missing edges, SDK errors, and interrupted waits raise an error.
Source configuration can already have changed when pulse verification fails.
The kernel fallback can select external/internal GPS, but cannot disable PPS.

Clock selection accepts `internal_clock` or `external_clock` (case insensitive)
and rejects other names before accessing the SDK. Actual source support is
card dependent; SDK failures are returned to the caller. The SDK host reference
retains the `internal_clock` alias. Reference rates describe configuration,
not measured clock presence or lock; zero rates are rejected. External-rate
readback depends on SDK/card support.

Device setting keys are case insensitive. `iq_swap` and `counter` accept
`true`/`false` (case insensitive) or `1`/`0`; other values raise an error.
`sys_clock_freq`, `full_scale`, `overload`, and `cal_offset` are read only.
Unknown keys raise an error. Metadata reports the current cached settings.
Counter mode applies to all RX handles and, per SDK requirements, takes effect
when RX streaming next starts. Failed multi-handle changes attempt to restore
all affected handles; rollback failures are reported and require recovery.
Timestamp selection accepts `rf_timestamp` or `sys_timestamp`; configure it
before starting RX streaming.

Stream tokens belong to the device that created them and become invalid on
close. Unknown tokens return `SOAPY_SDR_STREAM_ERROR` from activation,
deactivation, I/O, and status calls. `closeStream` and `getStreamMTU` raise
an argument error for an unknown token; MTU failures never return an unsigned
version of a negative error code.

RX activation reads the current actual sample rate and, when selected, the
current system timestamp frequency. Fractional sample rates retain their exact
binary value through the worker and partial-read timestamp calculation.
Configure rates, clocks, and timestamp selection before activating RX; stop
and reactivate RX after changing them so it captures a fresh timing snapshot.

`getHardwareTime` uses system time for the empty selector or `sys_timestamp`.
Explicit `rx_rf_timestamp` and `tx_rf_timestamp` read the first mapped handle
with the SDK's current actual rate, preserving fractional Hz. Missing rates,
unsupported directions, unknown selectors, and nanosecond overflow raise errors.

TX `readStreamStatus` waits for a new underrun count or returns
`SOAPY_SDR_TIMEOUT`. An underrun reports stream channel mask `1`, flags `0`,
and no timestamp. SDK errors, failed PPS startup, and shutdown return
`SOAPY_SDR_STREAM_ERROR`. Shutdown wakes status waiters and drains admitted
status calls before allowing stream cleanup. Each successful TX start resets
the host underrun baseline to match the SDK.

Stream activation/deactivation accepts flags `0` for immediate operation, or
`SOAPY_SDR_HAS_TIME` with `timeNs=0` for the driver's existing next-PPS behavior.
Absolute timed starts/stops, additional flags, and finite-burst activation
(`numElems != 0`) return `SOAPY_SDR_NOT_SUPPORTED`. TX writes accept flags `0`;
timed writes, end-of-burst, and fragment flags are unsupported. As in the
SoapySDR contract, `timeNs` is ignored when `HAS_TIME` is absent.

SDK stop failures from `deactivateStream()` return `SOAPY_SDR_STREAM_ERROR` and
retain stream ownership so the caller can retry. TX stops keep new writes
blocked after failure. `closeStream()` raises an exception if RX hardware
cannot be stopped and retains the stream for recovery; active TX must be
deactivated successfully before it can be closed.

## Sensors

`listSensors()` exposes `temperature` and `accelerometer`. `getSensorInfo()`
describes their types, units, and string formats without reading hardware.

| Key | Type | Units | `readSensor()` value |
| --- | --- | --- | --- |
| `temperature` | Integer | Degrees Celsius (`C`) | Signed decimal, for example `-12` |
| `accelerometer` | String | Thousandths of standard gravity (`mg`) | JSON object, for example `{"x":1,"y":-2,"z":1000}` |

Accelerometer axes retain the SDK's signed integer values; `1000` represents
one standard gravity. JSON uses `x`, `y`, and `z` numeric members and is independent
of the application's locale. The SDK 4.26 sensor API reports these units for all
supported products. Metadata does not imply that a card has an accelerometer:
the read checks support and returns `{}` when it is absent.

The existing read API logs SDK failures and returns `0` for temperature or `{}`
for acceleration. A temperature value of `0` alone therefore cannot distinguish
a valid reading from a failed read. Metadata requests for unknown keys raise
an argument error.

## License

- https://github.com/pothosware/SoapySidekiq/blob/master/LICENSE

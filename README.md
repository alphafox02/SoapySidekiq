# SoapySidekiq

SoapySDR hardware module for Epiq Sidekiq SDRs.

`SoapySidekiq` provides a SoapySDR device driver backed by `libsidekiq`.
It supports basic RX/TX operation, multi-handle receive, multi-card use,
runtime tuning and rate changes, RF-port selection through the Soapy antenna
API, profile-aware rate reporting, and direct TX attenuation control.

The SoapySDR device selector is:

```text
driver=sidekiq
```

## Requirements

- SoapySDR development files
- CMake 3.10 or newer
- A C++17 compiler
- Epiq Sidekiq SDK / `libsidekiq`
- Epiq runtime libraries available to the dynamic linker

For G20/G40 targets, build SoapySDR from source unless the target OS packages
provide the SoapySDR ABI you intend to use.

### Sidekiq SDK versions

SDK v4.26.0 and newer ship a `sidekiq-config` tool, which the build uses to
pick the right library and support libraries for the platform. Older SDKs are
still supported: when `sidekiq-config` is not present, the build falls back to
locating the library for the host platform directly (pass `-DPLATFORM=...` for
cross-platform SDK flavors such as `msiq-g20g40`).

Features that need a newer SDK are enabled at compile time based on the SDK
headers. Topology selection and the Matchstiq Z4 need SDK v4.26.0 or newer;
with an older SDK the `topology` device argument is accepted but ignored with
a warning.

### Locating the SDK

The build looks for the SDK in this order:

1. `-DSIDEKIQ_SDK_DIR=/path/to/sidekiq_sdk_current`
2. `SIDEKIQ_SDK_DIR` from the environment
3. `Sidekiq_DIR` from the environment (older name, still honored)
4. `$HOME/sidekiq_sdk_current`

Pass `-DSIDEKIQ_USE_SIDEKIQ_CONFIG=OFF` to skip `sidekiq-config` and use the
direct library search even with a newer SDK.

## Build And Install

Build from the SoapySidekiq checkout you intend to use.

```bash
export SIDEKIQ_SDK_DIR=$HOME/sidekiq_sdk_current
export LD_LIBRARY_PATH=/usr/local/lib:/usr/lib/epiq:${LD_LIBRARY_PATH}

cmake -S . -B build \
  -DCMAKE_PREFIX_PATH=/usr/local \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
sudo cmake --install build
sudo ldconfig
```

For a G20/G40 target:

```bash
cmake -S . -B build \
  -DPLATFORM=msiq-g20g40 \
  -DSIDEKIQ_SDK_DIR=/home/sidekiq/sidekiq_sdk_current \
  -DCMAKE_PREFIX_PATH=/usr/local \
  -DCMAKE_BUILD_TYPE=Release
```

To test a rebuilt module without replacing the system install, stage it with
`SOAPY_SDR_ROOT` before running `SoapySDRUtil`.

## Device Selection

List and probe Sidekiq devices:

```bash
SoapySDRUtil --find="driver=sidekiq"
SoapySDRUtil --probe="driver=sidekiq,card=0"
```

Open the first Sidekiq card:

```text
driver=sidekiq,card=0
```

Select by serial number:

```text
driver=sidekiq,serial=B124
```

Some applications, including common Gqrx Soapy flows, open one RX channel and
do not expose a separate channel selector. For those applications, include the
requested RX path in the device string with `rx_channel`:

```text
driver=sidekiq,card=0,rx_channel=1
soapy=0,driver=sidekiq,card=0,rx_channel=1
```

`channel=` remains accepted as a compatibility alias, but `rx_channel=` is the
clearer spelling for single-client RX path selection.

Soapy channel numbers select Sidekiq RX handles. Soapy antenna names select RF
ports for the chosen handle. Use `SoapySDRUtil --probe` to inspect the mapping
reported by the installed card, FPGA image, and `libsidekiq` runtime.

### GPS and GPSDO (Sidekiq Stretch)

On cards with an on-board GPS, the driver exposes the controls from Epiq's
`sidekiq_gps` kernel module (`/sys/fs/skiq_gps/<card>/`):

- `gps_antenna_bias` setting and device argument: the 3.3 V bias on the GPS
  antenna port for an active GPS antenna, e.g.
  `driver=sidekiq,card=0,gps_antenna_bias=true`
- `gps_power` setting: power to the GPS module
- `gps_fix` sensor: the GPS has a position fix

On cards that support a GPS-disciplined oscillator (SDK v4.15.0 or newer), the
`gpsdo` clock source disciplines the internal reference to GPS, and the
`gpsdo_locked` and `gpsdo_freq_accuracy` (ppm) sensors report its state.

The antenna bias is on by default, so an active GPS antenna works without any
setup. The sysfs entries are writable only by root; to let applications
running as a normal user change the GPS settings, install the provided udev
rule, which gives the `plugdev` group write access whenever the card appears.
`cmake --install` installs it to `/etc/udev/rules.d` (configure with
`-DINSTALL_UDEV_RULES=OFF` to skip it, or `-DUDEV_RULES_PATH=...` to change the
directory). To apply it without rebooting:

```bash
sudo udevadm control --reload
sudo udevadm trigger --action=bind --subsystem-match=platform --sysname-match='skiq_gps.*'
```

### Advanced channel settings

These per-channel settings (`writeSetting(direction, channel, key, value)`)
expose libsidekiq features that SoapySDR has no API for. List them with
`SoapySDRUtil --probe` or `getSettingInfo(direction, channel)`.

- Frequency hopping: `freq_tune_mode` (`standard`, `hop_immediate`,
  `hop_on_timestamp`), `freq_hop_list` (comma-separated Hz), `freq_hop_next`,
  `freq_hop_perform` (time in ns on the readStream() timebase, or 0 for now)
  and `freq_hop_current`. Set the tune mode and hop list before
  `activateStream()`; hops can then be performed while streaming. libsidekiq
  keeps one hop pending: writing the list makes its first entry pending, and
  before each `freq_hop_perform` write `freq_hop_next` with the entry to follow
  the pending one. NV100/NVM2 advance through the list on their own.

  ```python
  sdr.writeSetting(SOAPY_SDR_RX, 0, "freq_tune_mode", "hop_immediate")
  sdr.writeSetting(SOAPY_SDR_RX, 0, "freq_hop_list", "902e6,915e6,928e6")
  # ... setupStream / activateStream ...
  sdr.writeSetting(SOAPY_SDR_RX, 0, "freq_hop_next", "1")      # after 902 MHz comes 915 MHz
  sdr.writeSetting(SOAPY_SDR_RX, 0, "freq_hop_perform", "0")   # hop to 902 MHz now
  ```

- `rf_filter`: RF preselect filter (RX) or filter path (TX), chosen from the
  filters the card reports, or `auto` to follow the LO frequency (the default;
  any retune re-selects automatically).
- `fir_gain`, `fir_config` (read only) and `fir_coeffs`: the RFIC's digital FIR.
  The coefficients are shared by the RFIC's channels and replaced by any
  sample-rate or bandwidth change; Epiq advises against writing them.

Device settings on NV100/NVM2 (SDK v4.22.0 or newer): `user_cal_save`,
`user_cal_load` and `user_cal_clear` take a calibration name.

On Sidekiq X2, X4 and Matchstiq X40, a profile made with Analog Devices' profile
tool can be loaded at open with the `rfic_profile` device argument, e.g.
`driver=sidekiq,card=0,rfic_profile=/path/to/profile.txt`. The rate it sets is
kept until the application changes the sample rate or bandwidth itself, since
that replaces the profile.

### Topologies (Matchstiq Z4)

Cards that support topologies (SDK v4.26.0 or newer) can be opened with a
topology ID, which is applied before the channel mapping is read:

```text
driver=sidekiq,card=0,topology=1
```

On cards without topology support the argument is ignored with a warning. The
active topology can be read back with `readSetting("topology")`; it reports the
topology ID, `none`, or `unsupported`. The test scripts in `tests/` accept a
`--topology` option.

## Streaming And Controls

RX supports `CS16` and `CF32` streams. TX accepts `CS16` and `CF32`, with
`CF32` converted to Sidekiq `CS16` internally.

Multi-channel RX uses the non-conflicting RX handles reported by the Sidekiq
SDK. TX supports one channel per active TX stream.

The driver buffers received samples for applications that briefly fall
behind: 500 ms per channel at the stream's sample rate by default. Change it
with the `buffer_ms` stream argument, for example `buffer_ms=2000`. If the
application falls further behind than that, samples are dropped and the next
`readStream()` call at the gap returns `SOAPY_SDR_OVERFLOW`; the same happens
for any other discontinuity, such as retuning while streaming. Samples
returned by a single `readStream()` call are always contiguous.

Passing `SOAPY_SDR_HAS_TIME` to `activateStream()` starts the stream on the
next 1PPS edge. For TX, `writeStream()` accepts samples once the stream has
started; until then it waits up to its timeout and returns `SOAPY_SDR_TIMEOUT`,
so the call can simply be retried.

The `rx_channel` alias is intended to improve selected-path operation in
single-client applications such as Gqrx. It does not bypass libsidekiq card
ownership, so multiple independent processes still cannot open the same
Sidekiq card simultaneously.

Sample rates and bandwidths are reported through the Soapy range and list APIs.
On profile-based radios such as NV100/NVM2, the broad min/max range does not
mean every value in the range is a valid RFIC profile. Use `SoapySDRUtil
--probe` or `build/range_smoke --cards 0` to inspect concrete choices.

The read-only `full_scale` setting reports the card's full-scale integer sample
value (for example 2047 for a 12-bit card), for scaling `CS16` samples.

RX gain maps to the Sidekiq RX gain table. TX output power is controlled by
Sidekiq attenuation. The aggregate Soapy TX `setGain()` call uses gain-style
semantics; for direct Epiq attenuation semantics, use the named gain element:

```python
sdr.setGain(SOAPY_SDR_TX, channel, "attenuation", 10.0)
attenuation = sdr.getGain(SOAPY_SDR_TX, channel, "attenuation")
```

## Examples And Tests

The `tests/` directory contains Python examples and C++ smoke-test utilities:

- `test_api`: interactive Soapy API exercise
- `cs16_validate` / `cf32_validate`: counter-mode RX validation
- `txtone`: tone generation and transmit
- `rx_multi_cs16_validate`: multi-channel RX validation on one card
- `rx_multicard_cs16_validate`: multi-card RX validation
- `antenna_smoke`: RF-port listing and switching checks
- `range_smoke`: rate, bandwidth, frequency, gain, and native-scale checks

`unit_tests` checks the driver's internal helpers (rate and bandwidth tables,
gain conversions, handle mapping, validation) without any hardware:

```bash
ctest --test-dir build --label-exclude hardware
```

Configure with `-DSOAPYSIDEKIQ_ENABLE_HARDWARE_TESTS=ON` to also register the
smoke tests, which need an attached card.

`.github/workflows/build.yml` builds with and without `sidekiq-config` and runs
the unit tests. libsidekiq cannot be downloaded in CI, so it needs a
self-hosted runner with the Sidekiq SDK installed and labeled `sidekiq`, and it
only runs once the repository variable `SIDEKIQ_CI` is set to `true`. Pull
requests from forks are never run on that machine.

Common validation commands:

```bash
build/range_smoke --cards 0 --include-tx
build/antenna_smoke --cards 0
build/rx_multi_cs16_validate --card 0 --channels 0,1
```

## G20/G40 Notes

A G20/G40 carrier can present more than one Sidekiq card. A common four-channel
receive layout with two NV100 cards is:

- card 0, channel 0
- card 0, channel 1
- card 1, channel 0
- card 1, channel 1

This is represented as two Soapy devices with two RX channels each, not one
four-channel `card=0` device. Hardware capability still depends on the card,
FPGA image, and `libsidekiq` runtime.

## Troubleshooting

- If SoapySDR cannot find the module, confirm `libSidekiqSupport.so` is in the
  module path reported by `SoapySDRUtil --info`.
- If `libsidekiq` or Epiq support libraries cannot be found, verify
  `LD_LIBRARY_PATH` includes `/usr/lib/epiq` or the SDK runtime library path.
- If Gqrx shows sample rate or bandwidth as a numeric field rather than a
  dropdown, query valid choices with `SoapySDRUtil --probe` and enter a listed
  value manually.
- If a selected channel has unexpected RF ports, remember that stream channel
  selection and antenna/RF-port selection are separate Soapy concepts.
- If one Gqrx instance is already using a card, a second Gqrx process will
  still fail to open that same card until the first process releases the
  libsidekiq card lock.
- If changing sample rate or bandwidth fails on NV100/NVM2, use one of the
  listed RFIC profile rates and a compatible bandwidth.
- If a warning reports that the actual sample rate or bandwidth differs from
  the requested value, the RFIC could not produce the exact request; the
  warning shows the value the hardware is using.
- On products where 1PPS source selection has moved to the
  `epiq-axi-timing` kernel driver, the time source is read and written through
  `/sys/kernel/epiq-axi-timing/pps/source` when the libsidekiq calls fail.

## License And Attribution

This project is released under the Apache-2.0 license. See [LICENSE](LICENSE).

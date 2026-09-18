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

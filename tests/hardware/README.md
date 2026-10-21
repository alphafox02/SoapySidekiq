# Hardware validation programs

For the combined integration branch, follow the
[hardware integration test plan](INTEGRATION_TEST_PLAN.md). Save each run's
commands, card and RF setup, results, and capability skips with that run's
review or release evidence. The plan describes one integration node; check
this README and the selected cards' capabilities when using another node.

These programs require a Sidekiq card and, in some cases, additional equipment
or configuration such as an external PPS source or an RF loopback path.

The bounded validation programs return a nonzero exit status on a failed
assertion and close streams on exit. Run them one at a time against a card with
a supported sample rate. The interactive `test_api.py` program is still a
legacy diagnostic pending comparison with the non-hardware API test suite.

## Card and setup limits

This is not an automatic suite for every Sidekiq model or card combination.
Select a discovered card and supported channel, sample rate, bandwidth, and
gain settings before running each program. The single-card counter checks do
not need an RF input, but paired tone, full-duplex, and retune checks need
compatible cards and a verified RF path. PPS checks need an available PPS
source. Record unsupported combinations as skips rather than test failures.

Several paired validators default to the `8N55`/`47479` configuration used on
the integration node, including a 2 MS/s rate, 2047 RX host full scale, and a
short startup timeout. Those defaults do not fit every card. For example,
NV100 uses a 1.92 MS/s nominal SDK profile, 32767 CS16 host full scale, and
can take more than 6 seconds to initialize. Supply the relevant arguments
explicitly and check the card's capabilities before treating a result as a
driver assertion. `test_api.py` also contains fixed settings and is a manual
diagnostic, not a model-independent pass/fail test.

Current programs:

- `cf32_validate.py` checks converted CF32 counter samples.
- `cs16_validate.py` checks CS16 counter samples and timestamps.
- `run_loop.py` repeatedly activates and deactivates an RX stream.
- `partial_read_validate.py` checks counter order and timestamp phase when each
  read requests less than one stream MTU.
- `paired_tone_validate.py` coordinates bounded TX/RX processes on two cards
  and asserts received tone frequency and level from a saved capture.
- `full_duplex_pair_validate.py` runs one process per card and checks distinct
  peer tones in both directions while each card transmits and receives.
- `paired_retune_validate.py` holds a selected peer TX active in a separate
  process while PCIe RX retunes and checks both captured tone frequencies.
- `pps_lifecycle_validate.py` checks external-PPS RX/TX timed starts and stops,
  immediate cleanup after timed activation, and stream reuse. Its first timed
  TX write retries per-call timeouts for up to five seconds while PPS startup
  is pending.
- `nv100_multichannel_rx.cpp` checks NV100 two-channel CS16/CF32 counter data,
  RF/system timestamps, buffer order, loss recovery, and reactivation. It runs
  both A1/A2 and A1/B1 pairs, and rejects A2/B1. It never starts a TX stream.
- `test_api.py` interactively exercises device-control APIs.

The full-duplex detector searches actual spectral peaks within the larger of
20 kHz or two FFT bins of the expected peer frequency. It requires the peak
to be at least 10 dB above the median spectral noise over a five-bin main-lobe
bandwidth and within 40 dB of the
strongest tone in that capture, rejecting weak spurs from unrelated tones.
`--min-peer-relative-db` adjusts the latter threshold for a measured RF setup;
record any change with the run. This is a relative detection threshold, not
an absolute RF power measurement. Synthetic regressions cover the wrong-tone
quantization-spur failure and a weaker real peer alongside local leakage.

## Known integration limits

The driver currently supports one RX stream and one TX stream per device,
with one TX channel. RX accepts multiple non-conflicting handles at the same
actual sample rate and returns equal sample counts in requested buffer order.
Immediate multi-channel activation requires the SDK's synchronized RX start;
unsupported products fail that operation rather than falling back to an
unsynchronized start. An advertised handle does not establish that
every RX/TX handle pair can operate together. Confirm the SDK topology and RF
port mapping for the chosen pair.

On the integration node, the M.2-2280 (`8N55`) can crash during SDK
initialization if the PCIe card (`47479`) is already transmitting. The same
startup order failed with SDK prebuilt apps from versions 4.24–4.26, without
SoapySidekiq. Initialize `8N55` before starting PCIe TX for that pair.

The NV100 (`9YAA`) RX B1/J2 plus TX A1/J1 pair passed full duplex validation
at 1.92 MS/s. J2 TX did not produce a measurable tone in that node's RF
setup, including a direct SDK app check; its RF path remains unverified.
NV100's tested low, nominal, and high profiles were 1, 1.92, and 10 MS/s
with bandwidth set to 80% of the sample rate. A 2 MS/s request was rejected
by the SDK on this node.

The `8N55` high-output tone became distorted at some TX gain settings in the
tested RF path, including with direct SDK apps. Choose TX gain from measured
receiver level and waveform quality; an FFT peak alone is insufficient to
establish a clean tone. These observations do not establish calibrated RF
power or path attenuation.

For example, on a card supporting 2 MS/s:

```sh
python3 tests/hardware/cs16_validate.py --serial 47479 --rate 2e6
python3 tests/hardware/cf32_validate.py --serial 47479 --rate 2e6
python3 tests/hardware/run_loop.py --serial 47479 --rate 2e6
```

Replace `47479` and `2e6` with the selected card's serial and a supported
rate. Set `SOAPY_SDR_ROOT` to the intended module's install prefix when more
than one SoapySidekiq build is installed.

The counter tests use the FPGA's internal counter source, so an RF input is not
needed. `cs16_validate.py` checks both RF and system sample timestamps.

The bounded timestamp, simultaneous RX/TX, and RX retuning examples are in
[`examples/python`](../../examples/python/).

## NV100 multi-channel RX counter check

Build this standalone program against SoapySDR and select the module being
validated through the usual plugin environment:

```bash
c++ -std=c++17 -O2 tests/hardware/nv100_multichannel_rx.cpp \
  $(pkg-config --cflags --libs SoapySDR) -o /tmp/nv100_multichannel_rx
SOAPY_SDR_ROOT="$PWD/build/dev-install" \
SOAPY_SDR_PLUGIN_PATH="$PWD/build/dev-install/lib/SoapySDR/modules0.8" \
  timeout 240 /tmp/nv100_multichannel_rx driver=sidekiq,card=0
```

Use a released NV100 with three exposed RX handles. The program configures
5 MS/s, 4 MHz bandwidth and a 1.8 GHz receive LO, and restores the counter and
timestamp-source settings on normal or exception exits. Rate, bandwidth and
receive tuning remain at the test values. SDK profile calibration can take
several seconds. Run this program alone on the selected card.

Each of 16 cases verifies at least 65,792 complex samples per channel before
a 100 ms reader stall, checks shared overflow and aligned recovery, then
reactivates the same stream. RF timestamps are checked exactly; system-time
continuity allows one system-clock tick plus one nanosecond across hardware
block boundaries. The relative digital counter offset must remain constant
between the two planes, including across overflow recovery. Counter alignment
does not establish analog RF phase coherence. PPS and TX are separate checks.

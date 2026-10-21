# Non-hardware tests

Requires CMake 3.21+ and a C++17 compiler. From the repository root, use the
SDK-free `non-hardware`, `non-hardware-asan`, `non-hardware-tsan`, or
`non-hardware-coverage` configure/build/test presets described in the main
[README](../../README.md#presets). For example:

```sh
cmake --preset non-hardware
cmake --build --preset non-hardware --parallel 2
ctest --preset non-hardware
tests/non_hardware/run_suite.sh build/non-hardware
```

The standalone configure commands below remain available. Coverage adds
`--coverage` only to the portable targets. Inspect its `.gcda`/`.gcno` files
with matching GCC `gcov` or Clang `llvm-cov gcov` tools; host coverage does not
measure the SDK-dependent device implementation.

On Linux, the tooling tests configure and link tiny SDK archive fixtures to exercise SDK
selection, version/error handling, quoted compile flags, and static link order.
Install/uninstall cases use temporary prefixes and DESTDIR staging, and check
that unrelated files survive. The simulated ARM SDK metadata case does not
cross-compile an ARM binary. The finder runs `sidekiq-config` as a host tool even
when the toolchain redirects program searches into a target root. The selected
test compiler is used by the fixtures.

These tests do not open a Sidekiq card or link to the Sidekiq SDK. They live
alongside the hardware validation programs under `tests/`.

When Python 3 is available, CTest also runs `tooling.all`. Its installer cases
configure a tiny project with real CMake, using command shims to prevent driver
builds, installation, or privileged commands. They cover positional and
environment prefixes, typed variables, spaces, precedence, and stale caches.
Synthetic tone tests use NumPy to check the production full-duplex detector,
including rejection of a wrong-tone quantization spur. Those cases explicitly
skip when NumPy is unavailable. The Python cases are not sanitizer-instrumented;
the sanitizer configuration instruments the C++ executables.
If interpreter discovery fails, configure with
`-DPython3_EXECUTABLE=/path/to/python3`; CMake reports when tooling tests are omitted.

Run tooling cases directly with:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tests/non_hardware/tooling/run_tests.py
```

Configure and build:

```sh
cmake -S tests/non_hardware -B build/non_hardware
cmake --build build/non_hardware --parallel
```

To watch a run, start `tail -F out.txt` in another terminal, then run:

```sh
tests/run_logged.sh non_hardware_normal tests/non_hardware/run_suite.sh
```

The live output is `out.txt`, which points to `logs/current.log`; the runner
also keeps a per-run log in `logs/`. The suite writes its CTest output to
`build/non_hardware/suite.log`. The final line reports the number of
individual cases, rather than only the number of aggregate CTest executables.

The TX write regression executable compiles the `writeStream` method body
directly from `Streaming.cpp` against a minimal device and synchronous SDK
fixture. CMake regenerates that body when the production source changes and
fails configuration if the method boundaries cannot be found. These tests
compare every sample in three CF32 and CS16 packets. They exercise successful
input advancement, not hardware setup, allocation rollback, asynchronous
callbacks, queue-full handling, or conversion clipping policy. CF32 coverage
includes negative normalized components, zero, and both signed full-scale
endpoints. The ASan/UBSan configuration also enables `float-cast-overflow` and
stops on sanitizer errors.

The PPS write-gate regressions inject completed successful and failed startup
results. They verify that a failed start rejects repeated writes without SDK
submissions, while a successful start permits subsequent packets. They do not
exercise the blocking SDK startup call, activation/reset behavior, or real PPS
edges.

The TX fixture also extracts the production native-format method to check
separate RX/TX host scales. The SDK unpacked-sample table specifies X2/X4/X40
TX as upper-14-bit samples in a 16-bit word: their host scale is 32767, not
8191. Tests assert explicit native scales and signed half-amplitude values
(16384 for upper-14-bit/16-bit, 1024 for right-justified 12-bit) across three
packets. They do not execute the constructor's SDK product/width queries or
measure RF output on hardware.

Timestamp cases run both through the compiler-selected implementation and in
`timestamp_portable_tests`, which forces the portable integer fallback even
on hosts with `__int128`. Both cover truncation boundaries and signed output
overflow. On compilers with `__int128`, 10,000 deterministic input pairs also
check each implementation against a wide integer oracle.

RX queue tests retain raw RF/system timestamp phase across partial reads and
run through both native and forced portable arithmetic. A rational integer
oracle checks 1,000 blocks with eight partial reads each. The RX lifecycle
fixture compiles the production close/deactivate methods and RX activation
branch with the real RxStreamSession, an injected receive failure, and fake
SDK start/stop calls. It covers close/reuse, stop failure with retry, and
already-stopped hardware. Stream setup, actual SDK I/O, and destructor cleanup
are outside that fixture's coverage.

The session component cases additionally cover concurrent activation during
worker teardown, retained ownership after stop errors or exceptions, and
fractional timestamps through both RF and system modes. Their backend receive
calls have a bounded wait, matching the production receive timeout even when
a hardware stop fails.

RX lifecycle regressions also check mixed PPS/immediate start and stop modes,
immediate close/destructor cleanup after PPS activation, and retrying a failed
PPS stop with an immediate stop. Startup rollback explicitly requests an
immediate stop; worker-creation failure is not injected by these tests.

AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
cmake -S tests/non_hardware -B build/non_hardware-asan \
    -DSOAPYSIDEKIQ_TEST_ASAN=ON
cmake --build build/non_hardware-asan --parallel
tests/run_logged.sh non_hardware_asan \
    tests/non_hardware/run_suite.sh build/non_hardware-asan
```

The SDK card-manager tests exercise the production lifecycle coordinator with a
fake backend: one library initialization across cards, per-card disable, failed
second-card opens and constructor rollback, duplicate owners, concurrent opens,
and disable/shutdown failures. These establish ownership ordering without the
SDK; they do not replace a two-card capture in one process.

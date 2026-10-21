# Contributing

Keep each PR focused on one behavior change or one mechanical cleanup. Add a
regression test for a defect and keep functional changes separate from formatting,
renaming, or source splitting. Start new work from current master after its
predecessor merges; avoid long stacks.

## C++17 conventions

Use the [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines)
as design guidance and the reviewed portable code under `include/SoapySidekiq`
as the local naming and ownership reference:

- Use four spaces, Allman braces, unindented namespaces, and a 100-column target.
  Keep include order explicit; the formatter does not sort includes.
- Use `PascalCase` types, `camelCase` functions, and descriptive local names.
  Follow the surrounding member convention when editing existing classes; do
  not rename SDK/SoapySDR overrides or unrelated code as a side effect.
- Prefer RAII and standard containers. Make resource ownership and callback
  lifetime explicit. Use scoped locks and document shutdown/wakeup ordering.
- Validate at the API boundary, preserve state on failure, and check SDK status.
  Use explicit checked numeric conversions; include operation/card/channel and
  backend status in new errors. Preserve established streaming status contracts.
- Add file-level Doxygen headers to new production C++ files and document reusable
  APIs, ownership, concurrency, and hardware constraints. Explain reasons rather
  than restating each statement.

These are conventions for focused new work. Existing code has formatting and
warning debt; adopting the baseline does not authorize a whole-tree rewrite.

## Reproducible tool versions

The initial Linux CI baseline is Ubuntu 22.04, GCC **11**, Clang **14**,
clang-format **14**, and clang-tidy **14**, with C++17. Local validation used
GCC 11.4.0, LLVM 14.0.0, CMake 3.22.1, and Python 3.10. CI records installed
versions on each run; distribution patch versions may change. Use versioned
LLVM commands so a default system upgrade does not silently change formatting.

The module needs SoapySDR development files and Sidekiq SDK **4.26+**. Public
CI builds portable production components and tests without that proprietary SDK;
it does not establish that the module builds/links or that a card works. The
module and standalone tests both require CMake 3.21+. The build respects the
caller's compiler/toolchain selection; see the [presets](README.md#presets).

On Ubuntu 22.04, install the portable baseline dependencies with:

```sh
sudo apt-get install g++-11 clang-14 clang-format-14 clang-tidy-14 python3-numpy
```

## Formatting focused changes

[`.clang-format`](.clang-format) defines the layout using
[LLVM 14's options](https://releases.llvm.org/14.0.0/tools/clang/docs/ClangFormatStyleOptions.html).
Format only the file or ranges you are deliberately cleaning up:

```sh
clang-format-14 --style=file --lines=20:45 -i path/to/file.cpp
python3 tools/check_format.py --base origin/master
```

The checker uses the merge base with the requested revision and checks added or
modified `.cpp`/`.hpp` lines in the committed, staged, and unstaged diff. Stage
new files first so Git includes them. Deleted files and untouched layout debt
are excluded. Clang-format can adjust the enclosing construct around a changed
line; review the proposed diff. The checker prints that diff and never edits
files. Invalid configuration, missing tools, or an invalid base fail the check.

## Compiler warnings and static analysis

The module and portable tests share `cmake/ProjectWarnings.cmake`: GCC/Clang
receive `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow`.
Sidekiq/SoapySDR include directories are marked `SYSTEM` on the module target.
Warnings remain diagnostic: there is no global `-Werror`. Fix inherited warning
debt in focused follow-ups rather than hiding it with broad suppressions.

[`.clang-tidy`](.clang-tidy) enables a small set of correctness checks, with
project-header filtering and system-header diagnostics disabled. It does not
enable naming, modernization, or automatic fix passes. Generate a compile
database and run the same portable-production analysis as CI:

```sh
cmake -S tests/non_hardware -B build/analysis \
  -DCMAKE_CXX_COMPILER=clang++-14 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
run-clang-tidy-14 -p build/analysis -j 2 -quiet \
  -clang-tidy-binary clang-tidy-14 \
  '/(DeviceDiscovery|DeviceOptions|SensorReader|StreamConfiguration|RxSampleQueue|RxStreamSession|SdkCardManager)\.cpp$'
```

The explicit correctness checks fail CI when found; compiler warnings are still
reported without promoting the whole inherited warning baseline to errors.
For SDK-dependent files, configure a real module build with
`-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, then run, for example,
`clang-tidy-14 -p build/module Settings.cpp`. Never treat analysis of fake test
fixtures as analysis of the entire hardware-owning device.

## Tests and CI

Run the [non-hardware suite](tests/non_hardware/README.md) before pushing:

```sh
cmake -S tests/non_hardware -B build/non_hardware \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++-11 \
  -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build build/non_hardware --parallel 2
tests/non_hardware/run_suite.sh build/non_hardware
cmake -S tests/non_hardware -B build/non_hardware-asan \
  -DCMAKE_BUILD_TYPE=Debug -DSOAPYSIDEKIQ_TEST_ASAN=ON \
  -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build build/non_hardware-asan --parallel 2
tests/non_hardware/run_suite.sh build/non_hardware-asan
```

The root `non-hardware` and `non-hardware-asan` presets configure the same suite
without looking for the SDK. CI uses those presets. TSan and coverage have
separate presets too; a TSan build alone does not establish that its runtime
works on the host. SDK-selection/link-usage and staged-uninstall regressions
use small real-CMake fixtures, without requiring the proprietary SDK or root.

The workflow runs GCC 11, GCC 11 ASan/UBSan, Clang 14, changed-line formatting,
and portable static analysis on PRs and master pushes. It saves counted case
results and analysis output as artifacts. NumPy and clang-format are installed
so the Python tooling regressions run as well. TSan, the full SDK build, and
[hardware validation](tests/hardware/README.md) remain separate checks; rerun
the relevant hardware cases when production behavior changes.

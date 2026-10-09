# `scripts/` — Repo-level scripts

These scripts build, test, benchmark, and maintain the Aria framework. The
flagship sample application is **AriaTools**; use its own repository and build
instructions to run or develop the product sample.

## Overview

| Script | Platforms | Purpose |
| --- | --- | --- |
| [`build.py`](./build.py) | All (Python 3) | Portable configure/build/test entry for native, Qt, web, iOS and Android targets; `--toolchain msvc\|mingw` on Windows, `--dry-run` prints the plan. |
| [`dependencies.py`](./dependencies.py) | All (Python 3) | Shared dependency resolver (GitHub, sqlite.org, bellard.org providers); `resolve` and `update` subcommands. |
| [`bench.py`](./bench.py) | All (Python 3) | Benchmark toolchain: calibration `profile`, absolute-budget `check`, scenario CLI `scenarios`, paired `compare`, three-phase `validate` and `verify-release`. |
| [`docs.py`](./docs.py) | All (Python 3) | Documentation toolchain: Doxygen input `filter`, HTML `prepare`/`check`, docs `api-check` and the pinned `doxygen` installer. |
| [`tidy-gate.sh`](./tidy-gate.sh) | macOS/Linux | clang-tidy baseline gate; fails only on new debt vs `clang-tidy-baseline.txt`. |
| [`pick-ios-simulator.py`](./pick-ios-simulator.py) | macOS | Pick a known-good iPhone + iOS runtime pair for the simulator test job. |
| [`run-android-tests.py`](./run-android-tests.py) | All (Python 3, adb) | Deploy and run native tests on an Android emulator or device. |

## Build-tree layout

All generated artefacts live below `build/`:

```text
build/
├── ide/                    VSCode CMake Tools workspace
├── flavors/
│   ├── release/            default/release build
│   ├── debug/              debug build
│   ├── asan/               AddressSanitizer + UBSan
│   ├── tsan/               ThreadSanitizer
│   ├── tsan-gate/          release verification gate
│   ├── bench/              benchmark build
│   └── msvc/               Visual Studio build
├── unified/                build.py default trees (isolated per plan)
├── platforms/
│   └── android/            Android NDK cross-build
└── dist/
    ├── tree/               installed SDK layout
    └── archives/           release archives
```

`rm -rf build/` provides a complete clean slate while keeping CMake caches
isolated between build trees. Configuring straight into `build/` is rejected
by the build-tree guard in `CMakeLists.txt`.

## Build and test

```bash
python3 scripts/build.py --test                                # release + ctest
python3 scripts/build.py --config Debug                        # debug
python3 scripts/build.py --config Debug --cmake-arg=-DARIA_ENABLE_ASAN=ON --cmake-arg=-DARIA_ENABLE_UBSAN=ON --test
python3 scripts/build.py --config Debug --cmake-arg=-DARIA_ENABLE_TSAN=ON --test
python3 scripts/build.py --platform android --ndk <path>       # Android NDK
python3 scripts/build.py --toolchain msvc --test               # Windows MSVC
python3 scripts/build.py --toolchain mingw --test              # Windows MSYS2 UCRT64
```

Use `--dry-run` to inspect the complete command plan without touching disk.

Useful environment variables:

- `CC` / `CXX`: override the compiler on macOS/Linux or MSYS2.
- `QT_DIR`: point adapter builds at a Qt 6 installation matching the compiler.
- `ARIA_NO_QT6=1`: disable the Qt 6 adapter, including in an existing build cache.
- `ARIA_NO_APPKIT=1`: disable AppKit adapter detection on macOS.

## Maintenance

For benchmark and documentation gates:

```bash
python3 scripts/bench.py check
python3 scripts/docs.py api-check
```

## AriaTools

AriaTools is the single flagship sample application. It is intentionally kept
outside the framework build, so changes to its UI and product dependencies do
not alter framework build trees. Follow the AriaTools project README for
setup, build, run, and debugging instructions.

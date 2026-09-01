# `scripts/` — Repo-level scripts

These scripts bootstrap, build, test, package, and maintain the Aria framework.
The flagship sample application is **AriaTools**; use its own repository and
build instructions to run or develop the product sample.

## Overview

| Script | Platforms | Purpose |
| --- | --- | --- |
| [`init-project.sh`](./init-project.sh) / [`init-project.ps1`](./init-project.ps1) | macOS/Linux / Windows | Generate machine-local VSCode settings, framework/test launch profiles, and framework/tests/docs/benchmark tasks. |
| [`build.sh`](./build.sh) | macOS/Linux | Framework build, tests, sanitizers, Android cross-build, and packaging. |
| [`build.ps1`](./build.ps1) | Windows (MSYS2 UCRT64) | Framework build, tests, Android cross-build, and packaging with GCC/Ninja. |
| [`build-msvc.ps1`](./build-msvc.ps1) | Windows (MSVC) | Framework build, tests, sanitizers, and packaging with Visual Studio. |
| [`check-bench.sh`](./check-bench.sh) | macOS/Linux | Benchmark regression gate. |
| [`check-docs-api.sh`](./check-docs-api.sh) | macOS/Linux | Documentation API coverage check. |
| [`tidy-gate.sh`](./tidy-gate.sh) | macOS/Linux | clang-tidy baseline gate; fails only on new debt vs `clang-tidy-baseline.txt`. |
| [`pick-ios-simulator.py`](./pick-ios-simulator.py) | macOS | Pick a known-good iPhone + iOS runtime pair for the simulator test job. |
| [`sync-cmake.sh`](./sync-cmake.sh) | macOS/Linux | Report module `.cpp`/`.mm` sources missing from their `CMakeLists.txt`. |

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
├── platforms/
│   └── android/            Android NDK cross-build
└── dist/
    ├── tree/               installed SDK layout
    └── archives/           release archives
```

`rm -rf build/` provides a complete clean slate while keeping CMake caches
isolated between build flavors.

## Bootstrap

Run once after cloning, and rerun whenever the generated VSCode templates
change:

```bash
./scripts/init-project.sh
```

```powershell
.\scripts\init-project.ps1
```

The generated `.vscode/*.json` files are ignored by Git. Edit the templates in
`init-project.sh` or `init-project.ps1`, not the generated files.

Generated tasks cover framework configuration/build, ctest, sanitizer builds,
documentation, benchmarks, Android NDK, and clang-tidy. The only generated
launch profile debugs a selected framework test binary.

## Build and package

Default invocation builds Release, runs ctest, and creates the installed SDK
under `build/dist/tree/`:

```bash
./scripts/build.sh
./scripts/build.sh debug
./scripts/build.sh tests
./scripts/build.sh asan
./scripts/build.sh tsan
./scripts/build.sh tsan-gate
./scripts/build.sh android
./scripts/build.sh pack-zip
./scripts/build.sh clean
```

```powershell
# MSYS2 UCRT64
.\scripts\build.ps1
.\scripts\build.ps1 debug
.\scripts\build.ps1 tests
.\scripts\build.ps1 asan
.\scripts\build.ps1 android
.\scripts\build.ps1 pack-zip
.\scripts\build.ps1 clean

# Visual Studio / MSVC
.\scripts\build-msvc.ps1
.\scripts\build-msvc.ps1 debug
.\scripts\build-msvc.ps1 tests
.\scripts\build-msvc.ps1 asan
.\scripts\build-msvc.ps1 pack-zip
.\scripts\build-msvc.ps1 clean
```

Useful environment variables:

- `CC` / `CXX`: override the compiler on macOS/Linux or MSYS2.
- `QT_DIR`: point adapter builds at a Qt 6 installation.
- `ARIA_NO_QT6=1`: disable Qt 6 adapter detection.
- `ARIA_NO_APPKIT=1`: disable AppKit adapter detection on macOS.
- `JOBS=N`: set parallelism for `build.sh`.

## Maintenance

After adding implementation files below `modules/`, check their CMake source
registration:

```bash
./scripts/sync-cmake.sh
```

For benchmark and documentation gates:

```bash
./scripts/check-bench.sh
./scripts/check-docs-api.sh
```

## AriaTools

AriaTools is the single flagship sample application. It is intentionally kept
outside the framework build and project-initialization scripts, so changes to
its UI and product dependencies do not alter framework build trees. Follow the
AriaTools project README for setup, build, run, and debugging instructions.

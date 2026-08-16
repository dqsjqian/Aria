# `scripts/` — Repo-level scripts

> Aria's scripts are split strictly along "framework vs demo" lines:
>
> * The repo-level scripts in this folder build, test, and package the
>   **aria framework only** — they never build examples.
> * To run a demo, use the per-demo runner: `examples/<demo>/scripts/run.sh`
>   or `examples/<demo>/scripts/run.ps1`.

---

## Overview

| Script | Platforms | Purpose | Notes |
| --- | --- | --- | --- |
| [`init-project.sh`](./init-project.sh) / [`init-project.ps1`](./init-project.ps1) | mac/Linux / Win | One-shot IDE bootstrap | Generates `.vscode/` settings/tasks/launch/extensions/c_cpp_properties. **Generator** — do not hand-edit `.vscode/*.json`. |
| [`build.sh`](./build.sh) | mac/Linux | **Framework build + tests + packaging** (default does all three) | Examples and the Qt6 adapter are explicitly not built here. |
| [`build.ps1`](./build.ps1) | Windows (MSYS2 UCRT64) | Framework build + tests + packaging on the MSYS2 toolchain | Auto-detects MSYS2; for MSVC use `build-msvc.ps1`. |
| [`build-msvc.ps1`](./build-msvc.ps1) | Windows (MSVC / VS 2022) | Framework build + tests + packaging on MSVC | Auto-detects VS 2022 via `vswhere`; uses `build/flavors/msvc/`. |
| [`check-bench.sh`](./check-bench.sh) | mac/Linux | Nightly bench regression gate | Pairs with `docs/performance.md`. |
| [`sync-cmake.sh`](./sync-cmake.sh) | mac/Linux | Maintenance: register new `.cpp` files into the right `CMakeLists.txt` | Run after adding source files. |
| [`open-readme.sh`](./open-readme.sh) | macOS | Open `README.html` / `README.zh-CN.html` in the browser | Convenience. |

> Demos are NOT in this table — see [§2 Running demos](#2-running-demos) below.

## Build-tree layout

Aria centralises all build artefacts under a single `build/` root.
Each script writes to its own subdirectory, so `rm -rf build/` always
gives you a clean slate, and CMake caches never cross-contaminate
between flavors / demos:

```
build/
├── ide/                     VSCode CMake Tools workspace (Debug + tests)
├── flavors/                 command-line build flavors (scripts/build.{sh,ps1})
│   ├── release/             scripts/build.sh / scripts/build.sh release  (default)
│   ├── debug/               scripts/build.sh debug
│   ├── asan/                scripts/build.sh asan        (ASan + UBSan)
│   ├── tsan/                scripts/build.sh tsan
│   ├── tsan-gate/           scripts/build.sh tsan-gate
│   └── msvc/                scripts/build-msvc.ps1
├── platforms/               cross-compilation targets
│   └── android/            scripts/build.sh android (NDK cross-build)
├── examples/                per-demo isolated cmake caches
│   ├── 1-qt-showcase/       examples/1-qt-showcase/scripts/run.{sh,ps1}
│   ├── 2-macos-appkit-mvvm/ examples/2-macos-appkit-mvvm/scripts/run.sh
│   ├── 3-ios-oc-uikit-mvvm/ examples/3-ios-oc-uikit-mvvm/scripts/run.sh
│   └── 4-web-mvvm/          examples/4-web-mvvm/scripts/run.{sh,ps1}
└── dist/                    release artefacts (only when packaging)
    ├── tree/                install-prefix layout for `package-release`
    └── archives/            tar.gz / zip output for `package-archive`
```

Why this layout:
* `ide/` is owned exclusively by VSCode CMake Tools — Debug, the only
  place `compile_commands.json` lives. Never touched by the command-line
  build scripts.
* `flavors/<flavor>/` are command-line builds. `tests` is an alias of
  `release` — same artefacts, just runs ctest after.
* `platforms/android/` is the NDK cross-build for the JNI adapter.
  Other cross-compilation targets (e.g. WebAssembly) will live here too.
* `examples/<demo>/` keeps each demo's CMake cache isolated. Different
  demos enable different options (Qt6, HTTP, OpenSSL, …); shared cache
  would force a re-configure every time you switch demos.
* `dist/` only exists when you run `pack-zip` / `package`. The two
  sub-dirs are predictable: `tree/` is what consumers see; `archives/`
  is what you upload to a GitHub release.

---

## 0. Bootstrap · `init-project.sh` / `init-project.ps1`

Run once after cloning. Generates IDE config (VSCode recommended
extensions, CMake Tools settings, debug launch profiles, build/run
tasks).

```bash
# macOS / Linux
./scripts/init-project.sh

# Windows
.\scripts\init-project.ps1
```

Note: `.vscode/*.json` files are **generated artefacts** and gitignored.
Any manual edits will be overwritten the next time you run init. To
change IDE behaviour, edit the templates inside
`init-project.{sh,ps1}` instead.

### Generated VSCode tasks

The init scripts seed several quality-gate tasks alongside the
existing demo ones, so a developer never has to remember the exact
`cmake -DARIA_ENABLE_*` invocations:

| Task label                       | Platforms     | What it does                                                                                                                                            |
| -------------------------------- | ------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `aria: configure (ASan+UBSan)`   | macOS / Linux / Win | Configures `build/flavors/asan/` with `ARIA_ENABLE_ASAN=ON ARIA_ENABLE_UBSAN=ON`.                                                                       |
| `aria: ctest (ASan+UBSan)`       | macOS / Linux / Win | Builds `build/flavors/asan/` then runs ctest under ASan+UBSan. Mirrors the `macos / ASan+UBSan` job in `ci.yml`.                                        |
| `aria: configure (TSan)`         | macOS / Linux | Configures `build/flavors/tsan/` with `ARIA_ENABLE_TSAN=ON`. (Omitted from the Windows ps1 — MinGW does not ship a usable TSan runtime on Windows.)     |
| `aria: ctest (TSan)`             | macOS / Linux | Builds `build/flavors/tsan/` and runs ctest with `TSAN_OPTIONS='halt_on_error=1'`. Mirrors the `tsan / nightly` job in `nightly.yml`.                    |
| `aria: clang-tidy (all headers)` | macOS / Linux / Win | Builds the project with `CMAKE_EXPORT_COMPILE_COMMANDS=ON` into `build/ide/`, then runs `clang-tidy -p build/ide --warnings-as-errors='*'` over `modules/*/include/aria/**.hpp`. Mirrors the `clang-tidy gate` job in `ci.yml`. |
| `aria: build (Android NDK)`     | macOS / Linux | Cross-builds the JNI adapter into `build/platforms/android/` using the Android NDK toolchain. Requires `ANDROID_SDK_ROOT` or a standard SDK install. |

---

## 1. Whole-project build · `build.sh` / `build.ps1` / `build-msvc.ps1`

**Default (no args): build framework + run tests + package release ->
`build/flavors/release/` + `build/dist/tree/`.**

```bash
# macOS / Linux
./scripts/build.sh                 # Default: Release framework + tests + ctest + package
./scripts/build.sh debug           # Debug framework + tests
./scripts/build.sh tests           # Alias of default (Release + tests + ctest, no extra dir)
./scripts/build.sh asan            # Debug + AddressSanitizer + UBSan
./scripts/build.sh tsan            # Debug + ThreadSanitizer
./scripts/build.sh pack-zip        # Default flow + emit build/dist/archives/aria-*.tar.gz
./scripts/build.sh tsan-gate       # Pre-release TSan gate (CHANGELOG promises every release is TSan-clean)
./scripts/build.sh android         # Android NDK cross-build (JNI adapter) -> build/platforms/android/
./scripts/build.sh clean
```

```powershell
# Windows — MSYS2 UCRT64 (GCC + Ninja)
.\scripts\build.ps1                # Default: Release framework + tests + ctest + package
.\scripts\build.ps1 debug
.\scripts\build.ps1 tests
.\scripts\build.ps1 asan
.\scripts\build.ps1 pack-zip       # build\dist\archives\aria-*.zip
.\scripts\build.ps1 clean
```

```powershell
# Windows — MSVC / Visual Studio 2022
.\scripts\build-msvc.ps1           # Default: Release framework + tests + ctest + package (build/flavors/msvc/)
.\scripts\build-msvc.ps1 debug
.\scripts\build-msvc.ps1 tests
.\scripts\build-msvc.ps1 asan      # /fsanitize=address (MSVC has no UBSan)
.\scripts\build-msvc.ps1 pack-zip
.\scripts\build-msvc.ps1 clean
```

Default-flow output layout (the install tree under `build/dist/tree/`):

```
build/dist/tree/
├── include/aria/...   # Public headers
├── lib/               # Static libs + import libs
├── bin/               # Shared libs (Windows DLLs)
├── cmake/             # CMake package config for find_package
├── examples/          # Example source code (sources only, no demo build artefacts)
├── LICENSE
├── README.md / README.zh-CN.md
└── CHANGELOG.md
```

---

## 2. Running demos

Demos are launched through per-demo runners. Each runner reconfigures
CMake to enable examples and only builds what its demo needs.

| Demo | Runner | Platforms |
| --- | --- | --- |
| 1-qt-showcase (Qt 9-tab feature showcase) | `examples/1-qt-showcase/scripts/run.{sh,ps1}` | macOS / Linux / Windows |
| 2-macos-appkit-mvvm (AppKit MVVM) | `examples/2-macos-appkit-mvvm/scripts/run.sh` | macOS |
| 3-ios-oc-uikit-mvvm (UIKit MVVM, iOS Simulator) | `examples/3-ios-oc-uikit-mvvm/scripts/run.sh` | macOS host |
| 4-web-mvvm (Web MVVM via HTTP/SSE/HTTPS adapter) | `examples/4-web-mvvm/scripts/run.{sh,ps1}` | macOS / Linux / Windows |

**demo1 — Qt showcase**:

```bash
./examples/1-qt-showcase/scripts/run.sh                   # Debug + GUI
./examples/1-qt-showcase/scripts/run.sh Release           # Release + GUI
./examples/1-qt-showcase/scripts/run.sh Debug probe       # probe mode: cycles all 9 tabs and exits (CI-friendly)
./examples/1-qt-showcase/scripts/run.sh Debug --no-launch # build only, don't launch
```

```powershell
.\examples\1-qt-showcase\scripts\run.ps1                  # Debug + GUI
.\examples\1-qt-showcase\scripts\run.ps1 Release          # Release + GUI
.\examples\1-qt-showcase\scripts\run.ps1 Debug probe      # probe mode
.\examples\1-qt-showcase\scripts\run.ps1 Debug --no-launch
```

Environment variables: `QT_DIR=...` to override Qt detection (default
probes Homebrew `/opt/homebrew/opt/qt`); `JOBS=N` for parallelism.

**demo2 — AppKit**:

```bash
./examples/2-macos-appkit-mvvm/scripts/run.sh             # Debug: build + sign + open app
./examples/2-macos-appkit-mvvm/scripts/run.sh Release
./examples/2-macos-appkit-mvvm/scripts/run.sh Debug --no-launch
```

**demo3 — UIKit (iOS Simulator)**:

```bash
./examples/3-ios-oc-uikit-mvvm/scripts/run.sh             # Debug: build Aria for iOS Sim + xcodebuild + boot sim + install & launch
./examples/3-ios-oc-uikit-mvvm/scripts/run.sh Release
./examples/3-ios-oc-uikit-mvvm/scripts/run.sh Debug --no-launch
```

**demo4 — Web MVVM (HTTP / SSE / HTTPS)**:

```bash
./examples/4-web-mvvm/scripts/run.sh                      # Debug + HTTP on :19090
./examples/4-web-mvvm/scripts/run.sh Release              # Release + HTTP
./examples/4-web-mvvm/scripts/run.sh Debug --tls          # Debug + HTTPS (auto self-signed cert under build/examples/4-web-mvvm/_certs/)
./examples/4-web-mvvm/scripts/run.sh Debug --probe        # boot, curl /aria/health + /aria/views, exit (CI-friendly)
./examples/4-web-mvvm/scripts/run.sh Debug --no-launch    # build only
```

```powershell
.\examples\4-web-mvvm\scripts\run.ps1                     # Debug + HTTP
.\examples\4-web-mvvm\scripts\run.ps1 Release             # Release + HTTP
.\examples\4-web-mvvm\scripts\run.ps1 Debug --tls         # Debug + HTTPS
.\examples\4-web-mvvm\scripts\run.ps1 Debug --probe       # boot + probe
.\examples\4-web-mvvm\scripts\run.ps1 Debug --no-launch
```

Environment variables: `ARIA_DEMO4_PORT=...` (default 19090),
`ARIA_DEMO4_CERT=...` / `ARIA_DEMO4_KEY=...` to skip self-signed
cert generation when using `--tls`. First-time `--tls` configure
builds vendored OpenSSL 3.5 from source (~1-2 min).

---

## 3. Maintenance · `sync-cmake.sh`

After adding a new `.cpp` under `modules/<m>/src/`, register it into
the corresponding `CMakeLists.txt`:

```bash
./scripts/sync-cmake.sh
```

---

## 4. Convenience · `open-readme.sh`

```bash
./scripts/open-readme.sh
./scripts/open-readme.sh zh
./scripts/open-readme.sh en
```

---

## Common workflows

```bash
# First-time clone
./scripts/init-project.sh
./scripts/build.sh tests          # framework + ctest, no packaging
./examples/1-qt-showcase/scripts/run.sh    # peek at the Qt showcase

# Pre-commit
./scripts/build.sh tests          # framework + ctest must be green
./examples/1-qt-showcase/scripts/run.sh Debug probe   # 9-tab smoke test

# Pre-release
./scripts/build.sh tsan-gate      # TSan stress must be green
./scripts/build.sh pack-zip       # produce build/dist/tree/ + build/dist/archives/aria-*.tar.gz
```

## Why every repo-level script has multiple flavours

Aria's CI matrix covers macOS + Linux + Windows (both MSYS2 and MSVC).
Repo-level scripts (`init-project`, `build`) must run on every supported
host. Hence:

- `build.sh` / `init-project.sh` for macOS and Linux.
- `build.ps1` / `init-project.ps1` for Windows + MSYS2.
- `build-msvc.ps1` for Windows + MSVC (Visual Studio 2022).

Demo runners follow each demo's actual target platform: demo1 ships both
`.sh` and `.ps1` (cross-platform Qt); demo2/demo3 are macOS-host only
(`.sh`).

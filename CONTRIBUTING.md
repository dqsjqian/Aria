# Contributing to Aria

Thanks for your interest in improving Aria. This guide covers how to set up,
build, test, and submit changes.

## Ground rules

- **Framework first.** Per `docs/ROADMAP.md`, core contracts (reactive,
  async, binding, validation, derived collections, lifecycle, error model,
  diagnostics) must stay A-grade before adapter surface grows. New features
  land behind a pinned contract + tests, not ahead of them.
- **No new external runtime dependency** in the core/runtime/binding layers.
  Adapters may vendor single-header libs under `third_party/`.
- **Every change passes `ctest --output-on-failure`** on at least one of the
  supported toolchains before review.

## Prerequisites

- CMake >= 3.20
- A full C++20 compiler: GCC >= 12, Clang >= 15 (AppleClang 15+), or
  MSVC v143 (VS 2022)
- *(optional)* Qt6 >= 6.4 for the Qt adapter / showcase

## Build & test

```bash
cmake -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

**Use a single `build/` tree.** Toggle features with `-D` options on that
one directory rather than spawning a parallel build tree (`build-foo/`,
`build-ex/`, …) per feature — a second tree doubles configure/build/test
time and drifts out of sync. To change what gets built, just re-run
`cmake -B build` with the option flipped; CMake updates the existing cache
in place:

```bash
cmake -B build -DARIA_BUILD_EXAMPLES=ON   # add examples to the same tree
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Common options (all default to the value shown; flip on the existing
`build/`):

| Option | Default | Effect |
|---|---|---|
| `ARIA_BUILD_TESTS` | `ON` | unit/fuzz suites + `ctest` |
| `ARIA_BUILD_EXAMPLES` | `ON` | `examples/*` (Qt showcase auto-skips without `ARIA_BUILD_QT6`) |
| `ARIA_BUILD_BENCHMARK` | `ON` | `benchmark/` |
| `ARIA_BUILD_SHARED` | `ON` | runtime/binding as SHARED (required by the cross-dylib ABI smoke) |
| `ARIA_BUILD_QT6` | `OFF` | Qt6 adapter + `examples/1-qt-showcase` |
| `ARIA_BUILD_HTTP` | `OFF` | HTTP/REST/SSE adapter + `examples/4-web-mvvm` |
| `ARIA_ENABLE_ASAN` / `_UBSAN` / `_TSAN` | `OFF` | sanitizer passes (use a *throwaway* tree only here, e.g. `build-asan/`, since the flags change the ABI) |

> The one legitimate reason to keep a second tree is a sanitizer/ABI-altering
> build (`-DARIA_ENABLE_ASAN=ON` etc.), because those flags are not safe to
> mix with a normal build in the same cache.

One-liners:

```bash
scripts/build.sh tests      # release + ctest          (macOS/Linux)
scripts/build.sh asan       # debug + ASan + UBSan
scripts/build.sh tsan       # debug + ThreadSanitizer
scripts\build.ps1 tests     # MSYS2 UCRT64             (Windows)
scripts\build-msvc.ps1 tests# MSVC / VS 2022           (Windows)
```

Run the fuzzers (lifecycle / re-entrancy invariants) before touching the
reactive core:

```bash
cmake -B build -DARIA_ENABLE_ASAN=ON
cmake --build build -j --target aria_fuzz
ARIA_FUZZ_ITERS=200000 ./build/bin/aria_fuzz
```

## Code style

- Enforced by `.clang-format` and `.clang-tidy` — run both before pushing:

  ```bash
  clang-format -i $(git diff --name-only --diff-filter=ACM | grep -E '\.(hpp|cpp|h)$')
  clang-tidy -p build <changed files>
  ```

- Public symbols live in `aria::` (or `aria::reactive::` / `aria::async::` /
  `aria::binding::`). Users never qualify a constraint name with an
  implementation namespace (see `docs/reference/api-style.md`, S-1).
- Warnings are errors. Do not silence a warning without a one-line comment
  explaining why it is a false positive.

## Tests

- New behaviour requires tests in the matching `modules/*/tests/` suite.
- Behaviour that crosses a documented contract (lifecycle `L-N`, error
  `E-N`, list-diff `LD-N`, diagnostics `D-N`, api-style `S-N`,
  selection `SE-N`) must reference the contract ID in the test name or a
  comment.
- Performance-sensitive changes update `benchmark/` and, if they shift a
  baseline, `benchmark/thresholds.json` with justification.

## Submitting changes

1. Open an issue first for design changes (an RFC under `docs/rfc/` for
   anything that crosses a module boundary, changes the ABI, or replaces a
   load-bearing third-party choice — see `docs/rfc/README.md`).
2. Keep PRs focused; one logical change per PR.
3. Ensure `ctest --output-on-failure` is green; note which toolchain(s) you
   ran.
4. Update the relevant doc(s) under `docs/` in the same PR.

## Layering rules (do not violate)

```
adapters  →  binding  →  runtime
                 │   ↘     ↙
                 ↓    core  →  abi
               async  →  core  →  abi
```

- `binding` depends on `async` (for `ViewModelScope` / `Navigation`
  cancellation), but `BindingEngine` and its `bind_*` methods stay
  async-agnostic: they operate purely on `Property<T>` and never name an
  `AsyncCommand` type. Wire the async→binding integration through primitives
  like `BindingEngine::bind_view_lifetime`; do NOT add an `AsyncCommand`
  parameter or overload to the engine itself.
- `core` and `abi` have zero external dependencies.

## License

By contributing you agree your contributions are licensed under the project's
MIT License.

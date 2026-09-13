# C++23 evaluation

Assessment date: 2026-09-13.

**Keep C++20 as the minimum requirement. Validate builds in C++23 mode,
and adopt individual newer facilities only when they solve a demonstrated
problem across the supported toolchains.** Changing the language flag alone
does not repair ownership, cancellation, collection diffs, or reentrancy,
and does not establish a performance improvement.

## Benefits for Aria

| Facility | Potential use | Decision |
|---|---|---|
| `std::expected<T, Error>` | Explicit success/failure for new synchronous APIs | Useful when such an API is needed. Existing `Loadable<T>` also represents idle, loading, refreshing, and stale data; it cannot be replaced by a two-state result. |
| `std::move_only_function` | Owning callbacks with move-only captures | Useful, but unavailable in the tested Apple and Android standard libraries. It also does not provide the fixed-capacity, no-heap contract of `inplace_function`. |
| `std::generator` | Standardize the synchronous generator | Cannot currently replace Aria's generator on all target toolchains. It does not replace asynchronous `Task`, executors, or cancellation. |
| Explicit object parameters (`deducing this`) | Reduce some const/ref overload duplication | A modest maintenance benefit; it would raise the compiler floor for public headers. |
| `std::to_underlying`, optional monadic operations, range helpers | Shorter implementation code | Small local improvements; insufficient reason to raise every consumer's minimum standard. |

Aria's `function_ref` has a separate non-owning contract. Its standard-library
counterpart is a C++26 facility, so C++23 alone does not remove that component.

## Local compiler and linker probes

Each cell below was tested with a small translation unit that actually uses
the facility and links an executable. These are feature probes, not a claim
that every Aria test has run on every operating-system version.

| Toolchain / target | `expected` and `transform` | `to_underlying` | Explicit object parameters | `move_only_function` | `generator` |
|---|---|---|---|---|---|
| Apple Clang 21 / macOS arm64 | Pass | Pass | Pass | Unavailable | Unavailable |
| Apple Clang 21 / iOS simulator arm64, minimum iOS 15 | Pass | Pass | Pass | Unavailable | Unavailable |
| Android NDK r29, Clang 21 / arm64, API 21 | Pass | Pass | Pass | Unavailable | Unavailable |
| Android NDK r25b, Clang 14 / arm64, API 21 | Unavailable | Unavailable | Unavailable | Unavailable | Unavailable |

The newer toolchains used `-std=c++23`; r25b used its accepted spelling
`-std=c++2b`. Failure is the absence of the tested facility, not a claim that
the compiler rejects every C++23 program.

Apple distinguishes compiler support from facilities requiring the deployed
OS's `libc++.dylib`; each adopted feature needs a deployment-target check.
For example, its support table requires Xcode 16.3 for explicit object
parameters. [Apple C++ support](https://developer.apple.com/xcode/cpp/)

NDK r29 updates the LLVM toolchain to clang-r563880c. Its exact standard-library
feature set must still be probed; the release number is not a promise of full
C++23 conformance. [NDK r29 changelog](https://github.com/android/ndk/wiki/Changelog-r29)

## Windows and Linux

GCC's library provides `expected` and `move_only_function` from GCC 12.1,
and `expected` monadic operations from 13.1. Aria's GCC 13 CI therefore has
these facilities, but this does not establish availability in Clang's selected
library: Linux Clang can use either libstdc++ or libc++.
[libstdc++ feature status](https://gcc.gnu.org/onlinedocs/libstdc++/manual/status.html)

MSVC documents support per compiler and library feature, rather than treating
the standard as an indivisible capability. A Windows C++23 build must pin a
toolset and its supported `/std` mode, then run the framework and adapter tests.
[MSVC conformance table](https://learn.microsoft.com/en-us/cpp/overview/visual-cpp-language-conformance?view=msvc-170)

LLVM's current status table still lists gaps, including `std::generator`.
The Apple/NDK probe results above are therefore decisive for the facilities
Aria could otherwise replace. [libc++ C++23 status](https://libcxx.llvm.org/Status/Cxx23.html)

## Adoption policy

1. Preserve exported `cxx_std_20` requirements and the default C++20 build.
   Respect an explicit parent-project or command-line `CMAKE_CXX_STANDARD=23`.
2. Test C++20 and C++23 builds without conditionally changing public type
   layouts or exported signatures. Consumers of one binary must agree on
   its ABI, regardless of their language mode.
3. Evaluate each candidate with feature-test macros and compile/link/runtime
   probes for Windows, Linux, Android, and Apple deployment targets.
4. Raise the minimum only when a measured simplification, required feature,
   or performance improvement outweighs the consumer toolchain upgrade.
   Document any API/ABI migration as a release-level change.

No C++23-only public API or baseline migration is part of this assessment.

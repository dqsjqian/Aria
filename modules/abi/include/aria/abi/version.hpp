#pragma once

#include "aria/abi/export.hpp"

#define ARIA_VERSION_MAJOR 1
#define ARIA_VERSION_MINOR 2
#define ARIA_VERSION_PATCH 1

#define ARIA_VERSION_STRING "1.2.1"

#define ARIA_ABI_VERSION 1

namespace aria::abi {

constexpr int version_major = ARIA_VERSION_MAJOR;
constexpr int version_minor = ARIA_VERSION_MINOR;
constexpr int version_patch = ARIA_VERSION_PATCH;
constexpr int abi_version = ARIA_ABI_VERSION;
constexpr const char* version_string = ARIA_VERSION_STRING;

// ---------------------------------------------------------------------------
//Runtime ABI interrogation
//
//  The constants above are compile-time: they bake into the consumer, so they
//  describe the headers the consumer was BUILT against and can never reveal a
//  mismatch with the library it is actually LINKED to. That made
//  `ARIA_ABI_VERSION` decorative — nothing in the project read it.
//
//  These two functions are compiled into the shared library, so comparing
//  them against the constants detects exactly that mismatch.
// ---------------------------------------------------------------------------

/// ABI version of the loaded aria library. Compare against the
/// `ARIA_ABI_VERSION` your translation unit was compiled with.
[[nodiscard]] ARIA_ABI_API int runtime_abi_version() noexcept;

/// Release version string of the loaded aria library ("1.2.1").
[[nodiscard]] ARIA_ABI_API const char* runtime_version_string() noexcept;

/// True iff the loaded library's ABI version matches the headers this
/// translation unit was compiled against.
///
/// Intended for a startup assertion in hosts that load aria dynamically:
///
///     if (!aria::abi::abi_matches_headers()) {
///         std::fprintf(stderr, "aria ABI mismatch: headers %d, library %d\n",
///                      aria::abi::abi_version,
///                      aria::abi::runtime_abi_version());
///         return 1;
///     }
[[nodiscard]] inline bool abi_matches_headers() noexcept {
    return runtime_abi_version() == abi_version;
}

}  // namespace aria::abi

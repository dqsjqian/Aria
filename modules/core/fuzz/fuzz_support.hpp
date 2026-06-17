#pragma once

// ============================================================================
//  modules/core/fuzz/fuzz_support.hpp
// ----------------------------------------------------------------------------
//  Shared infrastructure for the framework-wide stress / fuzz suite.
//
//  Each fuzzer file in this directory pins down ONE invariant from
//  docs/lifecycle.md (L-N) by hammering the framework with `iters()`
//  random perturbations and asserting the invariant after every step.
//
//  Iteration count knob
//  --------------------
//  The default iteration count is intentionally modest (50k) so the
//  whole suite finishes in a couple of seconds on a developer laptop
//  and inside CI. Nightly / pre-release runs override it via the
//  `ARIA_FUZZ_ITERS` environment variable -- the lifecycle.md targets
//  (1M / fuzzer) are intended for that mode.
//
//  Per-fuzzer seed
//  ---------------
//  Each fuzzer obtains a deterministic seed by default (so a CI
//  failure is reproducible). Override via `ARIA_FUZZ_SEED` to explore
//  a different region of the state space.
// ============================================================================

#include <cstdint>
#include <cstdlib>
#include <random>
#include <string>

namespace aria::fuzz {

/// Default per-fuzzer iteration count. Cheap enough for `make test`,
/// strong enough to expose the kind of races the framework actually
/// promises not to have. Override via `ARIA_FUZZ_ITERS` env var.
inline constexpr std::size_t kDefaultIters = 50'000;

namespace {

/// Read an environment variable. Uses `getenv_s` on MSVC (no deprecation
/// warning) and `std::getenv` elsewhere.
[[nodiscard]] inline const char* read_env(const char* name) noexcept {
#ifdef _MSC_VER
    static thread_local char buffer[256];
    std::size_t len = 0;
    if (getenv_s(&len, buffer, sizeof(buffer), name) == 0 && len > 0) {
        return buffer;
    }
    return nullptr;
#else
    return std::getenv(name);
#endif
}

}  // unnamed namespace

[[nodiscard]] inline std::size_t iters() noexcept {
    if (const char* env = read_env("ARIA_FUZZ_ITERS")) {
        try {
            const auto v = std::stoull(env);
            if (v > 0) return static_cast<std::size_t>(v);
        } catch (...) { /* fall through to default */ }
    }
    return kDefaultIters;
}

[[nodiscard]] inline std::uint64_t seed(std::uint64_t fallback) noexcept {
    if (const char* env = read_env("ARIA_FUZZ_SEED")) {
        try {
            return std::stoull(env);
        } catch (...) { /* fall through */ }
    }
    return fallback;
}

/// Convenience: a fast PRNG seeded once per fuzzer.
class Rng {
public:
    explicit Rng(std::uint64_t s) : eng_(s) {}

    [[nodiscard]] std::uint32_t u32() noexcept {
        return std::uniform_int_distribution<std::uint32_t>{}(eng_);
    }
    [[nodiscard]] std::uint32_t u32(std::uint32_t lo, std::uint32_t hi) noexcept {
        return std::uniform_int_distribution<std::uint32_t>{lo, hi}(eng_);
    }
    [[nodiscard]] bool coin(double p = 0.5) noexcept {
        return std::bernoulli_distribution{p}(eng_);
    }

private:
    std::mt19937_64 eng_;
};

}  // namespace aria::fuzz

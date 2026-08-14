#pragma once

/// numeric_saturate.hpp — saturating narrowing helpers for adapters
/// whose native widget tops out at `int` / `double`.
///
/// Adapters (Qt6, AppKit, UIKit) implement the 64-bit overloads of
/// `IViewAdapter::set_int64` / `set_uint64` by forwarding to the
/// 32-bit native path. A naive `static_cast<int>(value)` silently
/// truncates the high bits — for a `Property<int64_t>` holding a
/// timestamp this can produce nonsense.
///
/// These helpers do the next best thing:
///   * clamp the value to `[INT_MIN, INT_MAX]`;
///   * on overflow, emit **at most one** `runtime::Logger::warn` entry
///     per `(op_label, direction)` tuple per process — so the truncation
///     is visible in production logs without flooding them when a slider
///     drag or counter increment repeatedly hits the limit.
///
/// Rate-limit contract
/// -------------------
///   * "direction" is one of {`over` (v > INT_MAX), `under` (v < INT_MIN)}.
///   * Dedup is keyed by `op_label + "|" + direction`. Different labels
///     (e.g. `"qt::set_int64"` vs `"qt::set_uint64"`) get independent
///     budgets; over- and under-flow on the same label also get
///     independent budgets.
///   * Storage is a process-wide `inline` set guarded by an `inline`
///     mutex. In SHARED builds where this header is included from
///     multiple dynamic libraries each library carries its own dedup
///     state — that is acceptable because adapter labels are namespaced
///     per platform (`qt::*` / `appkit::*` / `uikit::*`) and never
///     collide across dylibs.
///   * Hard cap of 256 distinct keys: once that ceiling is reached,
///     additional keys silently saturate without warning. This is a
///     belt-and-suspenders guard against pathological label generation
///     (e.g. a label that incorporates a counter); production code
///     must use stable labels.
///
/// Testing
/// -------
/// `reset_saturate_warning_dedup_for_testing()` clears the dedup state
/// so unit tests can verify both the "first call warns" and "second
/// call is silent" sides of the contract independently. Production
/// code must not call it.
///
/// They are header-only and free of platform deps so any adapter
/// translation unit can include them.

#include "aria/runtime/logger.hpp"

#include <climits>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

namespace aria::binding::detail {

namespace numeric_saturate_detail_ {

inline constexpr std::size_t kMaxDedupEntries = 256;

inline std::mutex& dedup_mutex_() noexcept {
    static std::mutex m;
    return m;
}

inline std::unordered_set<std::string>& dedup_set_() {
    static std::unordered_set<std::string> s;
    return s;
}

/// Returns true iff this is the first time we see `(op_label, direction)`
/// on this dylib's process slot. Caller is expected to log on `true` and
/// stay silent on `false`. `noexcept` because failure paths (allocation
/// failure inside the unordered_set) degrade to "warn anyway" — losing a
/// dedup hit is preferable to terminating from a saturating helper.
inline bool should_warn_(std::string_view op_label,
                         std::string_view direction) noexcept {
    try {
        std::string key;
        key.reserve(op_label.size() + 1 + direction.size());
        key.append(op_label).append("|").append(direction);

        std::lock_guard lk(dedup_mutex_());
        auto& s = dedup_set_();
        if (s.size() >= kMaxDedupEntries) {
            // Capacity exceeded: stay silent unconditionally.
            //
            // For *existing* keys this is the warn-once suppression we
            // want. For *new* keys arriving after the cap is hit, this
            // is a documented degradation — we drop the warning rather
            // than risk unbounded growth from pathological label
            // generators (see kMaxDedupEntries contract above).
            // Callers must use stable, namespaced labels (`qt::*` /
            // `appkit::*` / `uikit::*`) so this branch is unreachable
            // in well-formed builds.
            return false;
        }
        auto [_, inserted] = s.insert(std::move(key));
        return inserted;
    } catch (...) {
        // Allocation / lock failure — fall through to "warn this time".
        return true;
    }
}

inline void emit_overflow_warn_(std::string_view op_label,
                                std::string_view direction,
                                std::string_view kind,  // "int64" | "uint64"
                                long long signed_v,
                                unsigned long long unsigned_v,
                                bool is_signed) noexcept {
    if (!should_warn_(op_label, direction)) return;
    try {
        std::string msg;
        msg.reserve(64);
        msg.append(kind).append(" ");
        msg.append(is_signed ? std::to_string(signed_v)
                             : std::to_string(unsigned_v));
        msg.append(direction == std::string_view{"over"}
                       ? " > INT_MAX → saturated (warn-once)"
                       : " < INT_MIN → saturated (warn-once)");
        ::aria::runtime::Logger::instance().warn(op_label, msg);
    } catch (...) {
        // Logger sink failure is none of our business — the unified
        // callback boundary inside Logger handles its own fallbacks.
    }
}

}  // namespace numeric_saturate_detail_

/// Clamp `v` into `[INT_MIN, INT_MAX]` and warn at most once per
/// `(op_label, direction)` if it had to be truncated. `op_label` should
/// look like "qt::set_int64", "appkit::set_uint64" — used both as the
/// log category and as the dedup key prefix.
inline int saturate_int64_to_int(std::int64_t v,
                                 std::string_view op_label) noexcept {
    if (v > static_cast<std::int64_t>(INT_MAX)) {
        numeric_saturate_detail_::emit_overflow_warn_(
            op_label, "over", "int64",
            static_cast<long long>(v),
            0ULL, /*is_signed=*/true);
        return INT_MAX;
    }
    if (v < static_cast<std::int64_t>(INT_MIN)) {
        numeric_saturate_detail_::emit_overflow_warn_(
            op_label, "under", "int64",
            static_cast<long long>(v),
            0ULL, /*is_signed=*/true);
        return INT_MIN;
    }
    return static_cast<int>(v);
}

inline int saturate_uint64_to_int(std::uint64_t v,
                                  std::string_view op_label) noexcept {
    if (v > static_cast<std::uint64_t>(INT_MAX)) {
        numeric_saturate_detail_::emit_overflow_warn_(
            op_label, "over", "uint64",
            0LL,
            static_cast<unsigned long long>(v),
            /*is_signed=*/false);
        return INT_MAX;
    }
    return static_cast<int>(v);
}

/// Reverse direction: read an int back as uint64. Negative values
/// surface as 0 (a `Property<uint64_t>` should not see negatives but
/// the underlying widget can dial down past 0).
inline std::uint64_t int_to_uint64_clamped(int v) noexcept {
    return v < 0 ? std::uint64_t{0} : static_cast<std::uint64_t>(v);
}

/// Test-only: drop the dedup memory so a fresh test case can observe
/// the "first warn" path again. Production code must not call this.
inline void reset_saturate_warning_dedup_for_testing() noexcept {
    try {
        std::lock_guard lk(numeric_saturate_detail_::dedup_mutex_());
        numeric_saturate_detail_::dedup_set_().clear();
    } catch (...) {
        // best-effort
    }
}

}  // namespace aria::binding::detail

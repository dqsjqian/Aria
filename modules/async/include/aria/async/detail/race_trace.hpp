#pragma once

// race_trace.hpp — one publish helper for the async race arbitration
// events defined as D-31.1 in docs/reference/diagnostics.md.
//
// Every combinator that arbitrates a race (`with_timeout`, `when_any`,
// `when_any_cancellable`, `when_all`) routes through here rather than
// repeating the `has_trace_sink()` gate at each of its arbitration
// points. Two reasons this is a header and not an inline lambda per
// call site:
//
//   * AD2 compliance in one place. The payload — which owns two
//     std::strings — must not be constructed when nobody is listening,
//     and a helper makes that gate impossible to forget.
//   * The op spellings are the wire format consumers filter on. Having
//     one definition means a typo is a compile error rather than an
//     event no `op == "race_won"` filter will ever match.
//
// Publishing happens from whichever thread won the race (a timer thread
// for `race_timeout`, a worker for `race_won`); sinks already have to
// assume that (AD5). `publish_trace_unchecked` swallows sink exceptions,
// so these calls are safe on a `noexcept` path.

#include "aria/diagnostics.hpp"
#include "aria/error.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace aria::async::detail {

/// Arbitration op spellings. String literals rather than an enum: the
/// `trace::Async::op` field is a `std::string` on the wire, and these
/// are what consumers match on.
namespace race_op {
inline constexpr std::string_view kStart        = "race_start";
inline constexpr std::string_view kWon          = "race_won";
inline constexpr std::string_view kTimeout      = "race_timeout";
inline constexpr std::string_view kLoserCancel  = "race_loser_cancel";
inline constexpr std::string_view kParentCancel = "race_parent_cancel";
inline constexpr std::string_view kEnd          = "race_end";
}  // namespace race_op

/// Combinator labels for `trace::Async::source` (D-31.1). These name the
/// combinator, never the user's work.
namespace race_source {
inline constexpr std::string_view kWithTimeout        = "with_timeout";
inline constexpr std::string_view kWhenAny            = "when_any";
inline constexpr std::string_view kWhenAnyCancellable = "when_any_cancellable";
inline constexpr std::string_view kWhenAll            = "when_all";
}  // namespace race_source

/// Publish one arbitration event. `generation` carries the per-op number
/// documented in D-31.1 (participant count / winner index / loser count),
/// and `err` is populated only for the two failure outcomes.
///
/// Callers do NOT need their own `has_trace_sink()` gate — the check is
/// here, before the payload strings are built.
inline void publish_race_trace(std::string_view source,
                               std::string_view op,
                               std::uint64_t generation = 0,
                               std::optional<::aria::Error> err = std::nullopt) noexcept {
    if (!::aria::has_trace_sink()) return;
    ::aria::publish_trace_unchecked(
        ::aria::TraceCategory::Async,
        ::aria::trace::Async{std::string(source), std::string(op), generation},
        std::move(err));
}

}  // namespace aria::async::detail

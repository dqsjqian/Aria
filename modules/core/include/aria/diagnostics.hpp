#pragma once

// ============================================================================
//  aria/diagnostics.hpp
// ----------------------------------------------------------------------------
//  Unified diagnostic protocol for the Aria framework. Per
//  docs/diagnostics.md, every observable subsystem publishes
//  `TraceEvent`s through a single, optional, thread-safe sink:
//
//    - reactive graph flush  -> Category::Reactive
//    - async command         -> Category::Async
//    - async resource        -> Category::Async
//    - binding engine        -> Category::Binding
//    - command (sync)        -> Category::Command
//    - validator             -> Category::Validation
//    - observable list       -> Category::List
//
//  Design pillars
//  --------------
//   1. Zero overhead when no sink is installed: the publish path costs
//      one atomic load + one branch. No string is built, no allocation
//      is done.
//
//   2. Single value type (`TraceEvent`) so tooling consumes ONE shape;
//      heterogeneous payloads are boxed into a small `std::variant`.
//
//   3. Thread-safe sink registration: a sink may be installed /
//      replaced / cleared from any thread at any time. Concurrent
//      publishers see a consistent snapshot via `std::shared_ptr`.
//
//   4. Aria's own subsystems are ALLOWED to keep their own focused
//      tracers (e.g. `GraphInspector::install_flush_tracer`); the
//      unified sink is an additional fanout, NOT a replacement.
//
//  Per docs/api-style.md S-1 these names live in `aria::`.
// ============================================================================

#include "aria/abi/export.hpp"
#include "aria/error.hpp"
#include "aria/validation_key.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace aria {

// ---------------------------------------------------------------------------
//  TraceCategory
// ---------------------------------------------------------------------------

/// Coarse subsystem label. Routers / filters discriminate on this
/// before looking at any other field. Stable enumerator order; never
/// re-ordered, only appended at the end.
enum class TraceCategory : std::uint8_t {
    Reactive   = 0,   ///< Graph flush, push-color, pull-evaluate
    Async      = 1,   ///< AsyncCommand / AsyncResource lifecycle
    Binding    = 2,   ///< BindingEngine VM<->View dispatch
    Command    = 3,   ///< Synchronous Command<Args...> execution
    Validation = 4,   ///< Validator / FormValidator rule runs
    List       = 5,   ///< ObservableList / FilteredList / SortedList / MappedList
};

[[nodiscard]] inline std::string_view to_string(TraceCategory c) noexcept {
    switch (c) {
        case TraceCategory::Reactive:   return "Reactive";
        case TraceCategory::Async:      return "Async";
        case TraceCategory::Binding:    return "Binding";
        case TraceCategory::Command:    return "Command";
        case TraceCategory::Validation: return "Validation";
        case TraceCategory::List:       return "List";
    }
    return "TraceCategory?";
}

// ---------------------------------------------------------------------------
//  Per-category event payloads
// ---------------------------------------------------------------------------
//
//  Each category carries a small, focused payload. Adding a new field
//  is a backwards-compatible change (consumers ignore unknown fields);
//  removing a field is breaking and must surface in CHANGELOG.

namespace trace {

/// Reactive flush phases. Mirrors `GraphInspector::FlushEvent::Phase`
/// (see `aria/reactive/inspector.hpp`) so consumers familiar with that
/// surface need no relearning. The two are bridged at runtime by
/// `GraphInspector::install_flush_tracer`'s sink-fanout adaptor.
enum class ReactivePhase : std::uint8_t {
    FlushBegin  = 0,
    RoundBegin  = 1,
    Pull        = 2,
    SkipClean   = 3,
    Recomputed  = 4,
    RoundEnd    = 5,
    FlushEnd    = 6,
};

struct Reactive {
    ReactivePhase phase = ReactivePhase::FlushBegin;
    /// Optional debug name of the node being processed (Pull /
    /// SkipClean / Recomputed). Empty for round / flush boundaries.
    std::string   node_name;
    /// Round counter (1-based). 0 for FlushBegin / FlushEnd outer
    /// boundaries.
    int           round = 0;
    /// Only meaningful for `Recomputed`: did the cached value
    /// actually move (true) or did the upstream-version fast-path
    /// short-circuit (false)?
    bool          changed = false;
};

/// Async lifecycle. Covers AsyncCommand and AsyncResource uniformly;
/// the `op` text disambiguates which (e.g. "execute_start",
/// "fetch_start", "cache_hit", "dedupe", "cancelled", "completed").
struct Async {
    /// Subsystem tag, e.g. "AsyncCommand" or "AsyncResource".
    std::string source;
    /// Operation, e.g. "execute_start" / "execute_finish" /
    /// "cancel" / "fetch_start" / "fetch_finish" / "cache_hit" /
    /// "dedupe" / "stale_drop".
    std::string op;
    /// Optional invocation generation counter (AsyncResource gen,
    /// AsyncCommand inflight id). 0 when not applicable.
    std::uint64_t generation = 0;
};

/// Binding events. `op` is one of "vm_to_view" / "view_to_vm" /
/// "feedback_suppressed" / "view_destroyed_drop".
struct Binding {
/// Adapter platform name ("qt6", "appkit", "uikit", "fake", ...).
    std::string platform;
    /// Bind target description, free-form ("text", "bool", "int", ...).
    std::string target;
    /// What happened.
    std::string op;
};

/// Synchronous Command<Args...> events. `op` is one of "execute" /
/// "rejected_can_execute" / "can_execute_changed".
struct Command {
    std::string op;
};

/// Validation events. Kind / source / message are folded directly so
/// consumers can render without further lookup. `key` keeps the
/// `(field_path, rule_id)` locator alive even in trace pipelines.
struct Validation {
    /// Operation: "rule_pass" / "rule_fail" / "warning_pass" /
    /// "warning_fail" / "begin_pending" / "end_pending".
    std::string   op;
    ValidationKey key;
    /// Carried only for the *_fail variants; empty otherwise.
    std::string   message;
};

/// Observable-list mutation. `op` mirrors `ListChangeKind`
/// ("Insert" / "Remove" / "Replace" / "Move" / "Reset" /
/// "ItemChanged"); index reflects the post-mutation list state per
/// L-31.
struct List {
    std::string   op;
    std::size_t   index = 0;
    std::size_t   from_index = 0;   ///< Move-only; 0 otherwise.
    std::size_t   size_after = 0;
};

}  // namespace trace

/// Heterogeneous payload. Order matches `TraceCategory`.
using TracePayload = std::variant<
    trace::Reactive,
    trace::Async,
    trace::Binding,
    trace::Command,
    trace::Validation,
    trace::List>;

// ---------------------------------------------------------------------------
//  TraceEvent
// ---------------------------------------------------------------------------

/// One trace event. Constructed inline at the publish site (typically
/// on the stack); the sink takes a `const&` and is responsible for any
/// further copy / serialise it does. Optional `error` lets failure
/// events ride the same channel without forcing every event to carry
/// a (mostly empty) Error payload.
struct TraceEvent {
    /// Coarse routing label.
    TraceCategory                  category;
    /// Per-category payload.
    TracePayload                   payload;
    /// Wall-clock timestamp at publish time. Useful for log
    /// correlation; consumers that don't need it pay only one
    /// `steady_clock::now()` per publish (cheap on commodity CPUs).
    std::chrono::steady_clock::time_point time =
        std::chrono::steady_clock::now();
    /// Optional `Error` snapshot (for `*_fail` / `*_error` events).
    /// nullopt for happy-path events; populated factories carry it
    /// so consumers can route failure traces without fishing into
    /// the payload variant.
    std::optional<::aria::Error>   error;

    [[nodiscard]] std::string_view category_name() const noexcept {
        return to_string(category);
    }
};

// ---------------------------------------------------------------------------
//  TraceSink
// ---------------------------------------------------------------------------

/// User-facing sink shape: a callable that receives every published
/// event. Implementations should be cheap on the happy path: the
/// publish call site is on the framework's hot path (every Property
/// flush, every list mutation) so any non-trivial work in the sink
/// must be pushed off-thread by the user themselves.
using TraceSink = std::function<void(const TraceEvent&)>;

namespace detail {

// Global trace sink storage lives in aria_runtime DLL so all modules
// (exe + DLLs) share one instance.  See src/diagnostics_sink.cpp.
ARIA_CORE_API std::shared_ptr<TraceSink>& global_sink_storage_() noexcept;
ARIA_CORE_API std::mutex& global_sink_mutex_() noexcept;

/// Atomically swap the global sink. Returns the previous one
/// (`nullptr` if none). Used by `install_trace_sink` /
/// `clear_trace_sink` and by `ScopedTraceSink` for save/restore.
inline std::shared_ptr<TraceSink>
swap_global_sink_(std::shared_ptr<TraceSink> next) noexcept {
    std::lock_guard lk(global_sink_mutex_());
    auto& slot = global_sink_storage_();
    auto prev = std::move(slot);
    slot = std::move(next);
    return prev;
}

/// Lift the current sink into a strong reference for the duration of
/// the publish call. Returns `nullptr` if no sink is installed (the
/// hot path branch).
inline std::shared_ptr<TraceSink> snapshot_global_sink_() noexcept {
    std::lock_guard lk(global_sink_mutex_());
    return global_sink_storage_();
}

}  // namespace detail

// ---------------------------------------------------------------------------
//  Public API: install / clear / publish
// ---------------------------------------------------------------------------

/// Install (or replace) the global sink. Safe to call from any
/// thread. Pass `{}` to clear; identical to `clear_trace_sink()`.
inline void install_trace_sink(TraceSink sink) {
    detail::swap_global_sink_(
        sink ? std::make_shared<TraceSink>(std::move(sink))
             : std::shared_ptr<TraceSink>{});
}

/// Tear down the global sink. After this returns, `publish()` is a
/// pure no-op until the next install.
inline void clear_trace_sink() noexcept {
    detail::swap_global_sink_({});
}

/// True iff a sink is currently installed. Cheap; useful for sites
/// that want to skip building a payload entirely when no one is
/// listening.
[[nodiscard]] inline bool has_trace_sink() noexcept {
    return static_cast<bool>(detail::snapshot_global_sink_());
}

/// Publish an already-built event.
///
/// **Performance contract (D-1)**: this function performs exactly
/// **one** `shared_ptr` load (the sink snapshot). When no sink is
/// installed the load yields nullptr and we return immediately --
/// hence the famed "one load + null check" zero-cost happy path.
///
/// Hot-path call sites that already gated on `has_trace_sink()`
/// should prefer `publish_trace_unchecked(...)` to avoid the
/// redundant second load on the slow path. The call site keeps
/// **one** load total in the slow path; the unchecked overload
/// trusts the caller's gate and just dispatches.
inline void publish_trace(const TraceEvent& ev) noexcept {
    auto sink = detail::snapshot_global_sink_();
    if (!sink || !*sink) return;
    try {
        (*sink)(ev);
    } catch (...) {
        // Sinks are diagnostic; never propagate.
    }
}

/// Slow-path companion to `publish_trace`. Skips the internal
/// has_trace_sink() check entirely and trusts the caller to have
/// already gated. Used by hot-path call sites that follow the
/// idiom:
///
///     if (::aria::has_trace_sink()) {
///         ::aria::publish_trace_unchecked(...);   // D-1: one load total
///     }
///
/// Performs exactly one `shared_ptr` load. Safe to call even when
/// no sink is installed -- it is just a wasted load + null check
/// (no UB).
inline void publish_trace_unchecked(const TraceEvent& ev) noexcept {
    auto sink = detail::snapshot_global_sink_();
    if (!sink || !*sink) return;
    try {
        (*sink)(ev);
    } catch (...) {
    }
}

/// Variadic convenience that builds a TraceEvent in place. Avoids
/// the extra std::optional / chrono noise at the call site.
///
/// **NOTE (D-1)**: this overload contains its own
/// `has_trace_sink()` short-circuit so it can be called
/// unconditionally from cold paths without the caller worrying
/// about gating. Hot paths that already gated should use the
/// unchecked counterpart below.
template<class Payload>
    requires (
        std::is_same_v<Payload, trace::Reactive>   ||
        std::is_same_v<Payload, trace::Async>      ||
        std::is_same_v<Payload, trace::Binding>    ||
        std::is_same_v<Payload, trace::Command>    ||
        std::is_same_v<Payload, trace::Validation> ||
        std::is_same_v<Payload, trace::List>)
inline void publish_trace(TraceCategory cat, Payload payload,
                          std::optional<::aria::Error> err = std::nullopt) {
    auto sink = detail::snapshot_global_sink_();
    if (!sink || !*sink) return;
    TraceEvent ev{cat, TracePayload{std::move(payload)},
                  std::chrono::steady_clock::now(), std::move(err)};
    try {
        (*sink)(ev);
    } catch (...) {
    }
}

/// Unchecked variadic counterpart -- builds the event and
/// dispatches with **one** sink snapshot. Trusts caller gating.
template<class Payload>
    requires (
        std::is_same_v<Payload, trace::Reactive>   ||
        std::is_same_v<Payload, trace::Async>      ||
        std::is_same_v<Payload, trace::Binding>    ||
        std::is_same_v<Payload, trace::Command>    ||
        std::is_same_v<Payload, trace::Validation> ||
        std::is_same_v<Payload, trace::List>)
inline void publish_trace_unchecked(TraceCategory cat, Payload payload,
                                    std::optional<::aria::Error> err = std::nullopt) {
    auto sink = detail::snapshot_global_sink_();
    if (!sink || !*sink) return;
    TraceEvent ev{cat, TracePayload{std::move(payload)},
                  std::chrono::steady_clock::now(), std::move(err)};
    try {
        (*sink)(ev);
    } catch (...) {
    }
}

// ---------------------------------------------------------------------------
//  ScopedTraceSink -- RAII install/restore for tests
// ---------------------------------------------------------------------------

/// Installs a sink for the lifetime of the scope, restoring whatever
/// was previously installed (possibly nothing) on destruction. The
/// idiomatic test harness:
///
///     std::vector<TraceEvent> log;
///     ScopedTraceSink guard{
///         [&](const TraceEvent& ev) { log.push_back(ev); }
///     };
///     // ... exercise the framework, then assert on `log` ...
class ScopedTraceSink {
public:
    explicit ScopedTraceSink(TraceSink sink)
        : previous_(detail::swap_global_sink_(
              sink ? std::make_shared<TraceSink>(std::move(sink))
                   : std::shared_ptr<TraceSink>{})) {}

    ~ScopedTraceSink() noexcept {
        detail::swap_global_sink_(std::move(previous_));
    }

    ScopedTraceSink(const ScopedTraceSink&)            = delete;
    ScopedTraceSink& operator=(const ScopedTraceSink&) = delete;
    ScopedTraceSink(ScopedTraceSink&&)                 = delete;
    ScopedTraceSink& operator=(ScopedTraceSink&&)      = delete;

private:
    std::shared_ptr<TraceSink> previous_;
};

}  // namespace aria

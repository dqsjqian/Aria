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
//   1. A cheap disabled path: the has_trace_sink gate costs
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
#include <concepts>
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
    /// Monotonic timestamp at publish time. Useful for event
    /// ordering; consumers that don't need it pay only one
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

// Shared trace storage lives in aria_abi, which is linked by core-only
// consumers and platform modules. See modules/abi/src/diagnostics.cpp.
ARIA_ABI_API std::shared_ptr<TraceSink>& global_sink_storage_() noexcept;
ARIA_ABI_API std::mutex& global_sink_mutex_() noexcept;
ARIA_ABI_API std::atomic<bool>& trace_sink_present_() noexcept;
ARIA_ABI_API void dispatch_trace_(const std::shared_ptr<TraceSink>& sink,
                                   const TraceEvent& event) noexcept;

/// Atomically swap the global sink. Returns the previous one
/// (`nullptr` if none). Used by `install_trace_sink` /
/// `clear_trace_sink` and by `ScopedTraceSink` for save/restore.
inline std::shared_ptr<TraceSink>
swap_global_sink_(std::shared_ptr<TraceSink> next) noexcept {
    std::lock_guard lk(global_sink_mutex_());
    auto& slot = global_sink_storage_();
    auto prev = std::move(slot);
    slot = std::move(next);
    trace_sink_present_().store(static_cast<bool>(slot), std::memory_order_release);
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
    return detail::trace_sink_present_().load(std::memory_order_acquire);
}

/// Publish an already-built event using one owning sink snapshot. Callers
/// normally gate expensive payload construction with has_trace_sink(). This
/// overload remains safe if the sink is removed after that check.
inline void publish_trace_unchecked(const TraceEvent& event) noexcept {
    auto sink = detail::snapshot_global_sink_();
    if (sink && *sink) detail::dispatch_trace_(sink, event);
}

/// Publish an already-built event. With no sink, only the atomic presence
/// flag is read. An enabled sink is retained under the registry mutex and
/// invoked outside it. Recursive traces are suppressed; sink exceptions are
/// reported through the callback-failure boundary.
inline void publish_trace(const TraceEvent& event) noexcept {
    if (has_trace_sink()) publish_trace_unchecked(event);
}

namespace detail {
template<class Payload>
concept TracePayloadType =
    std::same_as<Payload, trace::Reactive> || std::same_as<Payload, trace::Async> ||
    std::same_as<Payload, trace::Binding> || std::same_as<Payload, trace::Command> ||
    std::same_as<Payload, trace::Validation> || std::same_as<Payload, trace::List>;
}  // namespace detail

/// Build an event after taking one owning sink snapshot. Use after a
/// has_trace_sink() gate to avoid building expensive payloads when disabled.
template<detail::TracePayloadType Payload>
inline void publish_trace_unchecked(TraceCategory category, Payload payload,
                                    std::optional<::aria::Error> error = std::nullopt) {
    auto sink = detail::snapshot_global_sink_();
    if (!sink || !*sink) return;
    const TraceEvent event{category, TracePayload{std::move(payload)},
                           std::chrono::steady_clock::now(), std::move(error)};
    detail::dispatch_trace_(sink, event);
}

/// Convenience form for callers that already have a payload. To defer its
/// construction, place has_trace_sink() before constructing the argument.
template<detail::TracePayloadType Payload>
inline void publish_trace(TraceCategory category, Payload payload,
                          std::optional<::aria::Error> error = std::nullopt) {
    if (has_trace_sink())
        publish_trace_unchecked(category, std::move(payload), std::move(error));
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

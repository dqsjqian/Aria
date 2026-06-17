#pragma once

// IScheduler — unified scheduling base.
//
// Aria has historically grown three sibling abstractions:
//
//   * `aria::async::IExecutor`       — "post(fn)"; used by coroutines /
//                                       async_command / graph executor.
//   * `aria::runtime::IDispatcher`   — "post(fn) + post_delayed + pump +
//                                       is_main_thread"; the platform
//                                       UI/main-thread dispatcher.
//   * `aria::IDelayedScheduler`      — "post_after(delay, fn)"; the tiny
//                                       timer interface used by debounce
//                                       / throttle / retry / with_timeout.
//
// Each grew its own naming, its own role, and its own capability story.
// `IScheduler` is the single inheritance root that lets every component
// in the framework reason about "what can this thing do?" through a
// stable, declarative bitmask — without breaking any existing concrete
// class hierarchy.
//
//   ┌────────────────────────────┐
//   │       IScheduler           │  caps(): SchedulerCaps  bitmask
//   │       schedule(fn)         │  pure virtual — submit a callable
//   │       schedule_after(...)  │  default: throws unsupported_capability
//   └─────────────┬──────────────┘
//                 │
//   ┌─────────────┼─────────────────────────────┐
//   │             │                             │
//   IDelayedScheduler        IExecutor                  IDispatcher
//   (Caps::Delay)            (Caps::Post + GraphSafe?   (Caps::Post |
//                              + WorkerSafe?)             Caps::Delay |
//                                                         Caps::MainThread |
//                                                         Caps::Pumpable)
//
// All three derived interfaces inherit from `IScheduler` virtually so
// that `VirtualTimeExecutor : IExecutor, IDelayedScheduler` (and any
// future multi-role implementation) collapses to a single IScheduler
// subobject without ambiguity.
//
// The contract is intentionally minimal: `schedule(fn)` is the only
// universally available operation. Anything else (delay, main-thread
// affinity, pumping, pool-style worker hosting) is a CAPABILITY that
// the implementation declares via `caps()`. Callers query the bitmask
// once and either (a) use a richer interface via `dynamic_cast`, or
// (b) gracefully degrade.
//
// USAGE FROM A COMPONENT THAT NEEDS A TIMER:
//
//   void wire_debounce(IScheduler& s, ...) {
//       if (!has_caps(s, SchedulerCaps::Delay))
//           throw std::logic_error("scheduler has no Delay capability");
//       s.schedule_after(300ms, []{ ... });   // safe — caps() said yes
//   }
//
// USAGE FROM A COMPONENT THAT REQUIRES A PUMPABLE MAIN-THREAD QUEUE:
//
//   if (!has_caps(s, SchedulerCaps::MainThread | SchedulerCaps::Pumpable))
//       throw ...;
//
// IMPORTANT: This header introduces *no* breaking change. Every
// pre-existing concrete class (`ThreadPoolExecutor`, `MainThreadExecutor`,
// `InlineExecutor`, `SimpleDispatcher`, `VirtualTimeExecutor`, …) is
// retrofitted with a `caps()` override that declares its capabilities.
// Existing call sites keep using the rich interfaces directly.

#include "aria/abi/export.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <type_traits>

namespace aria {

// ────────────────────────────────────────────────────────────────────────
// Capability bitmask
//
// `enum class : std::uint32_t` so it can be stored as a packed atomic
// or a struct member without ABI surprises.
// ────────────────────────────────────────────────────────────────────────
enum class SchedulerCaps : std::uint32_t {
    None              = 0,

    /// Can submit "fire now" work. Every conforming `IScheduler`
    /// supplies this (the pure virtual `schedule()`); declared as a
    /// capability for symmetry / introspection.
    Post              = 1u << 0,

    /// Can submit work after a wall-clock or virtual-time delay.
    /// Implementation must override `schedule_after`.
    Delay             = 1u << 1,

    /// Submitted work runs on a single, identifiable "main" thread that
    /// is consistent across calls. `is_main_thread()` is meaningful.
    MainThread        = 1u << 2,

    /// Posted work is held in a queue until a pump-style call drains
    /// it (e.g. `pump`/`drain`/`run_one`). Components that need
    /// deterministic test-driven progress should require this.
    Pumpable          = 1u << 3,

    /// Safe to use as the *graph-thread executor* — i.e. work posted
    /// here will not race the reactive graph's owner-thread invariant.
    /// Equivalent to the historical `is_safe_graph_executor()`.
    GraphSafe         = 1u << 4,

    /// Safe to host blocking worker tasks (e.g. backed by a thread
    /// pool). Equivalent to the historical `is_safe_worker_executor()`.
    WorkerSafe        = 1u << 5,

    /// Implementation does not require any external pump and runs work
    /// on background threads autonomously. Useful for tests that want
    /// to assert "this is a thread pool, not a UI dispatcher".
    Autonomous        = 1u << 6,
};

[[nodiscard]] constexpr SchedulerCaps operator|(SchedulerCaps a, SchedulerCaps b) noexcept {
    return static_cast<SchedulerCaps>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr SchedulerCaps operator&(SchedulerCaps a, SchedulerCaps b) noexcept {
    return static_cast<SchedulerCaps>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}
constexpr SchedulerCaps& operator|=(SchedulerCaps& a, SchedulerCaps b) noexcept {
    a = a | b; return a;
}
constexpr SchedulerCaps& operator&=(SchedulerCaps& a, SchedulerCaps b) noexcept {
    a = a & b; return a;
}
[[nodiscard]] constexpr bool has_any(SchedulerCaps a, SchedulerCaps b) noexcept {
    return static_cast<std::uint32_t>(a & b) != 0;
}
[[nodiscard]] constexpr bool has_all(SchedulerCaps a, SchedulerCaps b) noexcept {
    return (a & b) == b;
}

// ────────────────────────────────────────────────────────────────────────
// Exceptions
// ────────────────────────────────────────────────────────────────────────

/// Thrown when a caller invokes a capability the scheduler did not
/// advertise (e.g. `schedule_after` on a non-Delay scheduler).
class unsupported_capability : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

// ────────────────────────────────────────────────────────────────────────
// IScheduler
// ────────────────────────────────────────────────────────────────────────
class ARIA_CORE_API IScheduler {
public:
    virtual ~IScheduler() = default;

    /// Capability bitmask. Implementations should return a constant
    /// value (or at least a value stable across the object's lifetime).
    [[nodiscard]] virtual SchedulerCaps caps() const noexcept = 0;

    /// Submit `fn` for execution "soon". Defines `Caps::Post`.
    virtual void schedule(std::function<void()> fn) = 0;

    /// Submit `fn` for execution after `delay`. Default implementation
    /// throws `unsupported_capability`; implementations that advertise
    /// `Caps::Delay` MUST override.
    virtual void schedule_after(std::chrono::milliseconds delay,
                                std::function<void()> fn) {
        (void)delay; (void)fn;
        throw unsupported_capability(
            "IScheduler::schedule_after: this scheduler does not advertise "
            "SchedulerCaps::Delay");
    }

    /// True iff the calling thread is the scheduler's "main" thread.
    /// Default returns false; implementations advertising
    /// `Caps::MainThread` SHOULD override with a meaningful answer.
    [[nodiscard]] virtual bool is_main_thread() const noexcept { return false; }
};

// ────────────────────────────────────────────────────────────────────────
// Free helpers
// ────────────────────────────────────────────────────────────────────────

[[nodiscard]] inline bool has_caps(const IScheduler& s, SchedulerCaps required) noexcept {
    return has_all(s.caps(), required);
}

/// Throwing accessor — useful at component construction time when
/// missing a capability is a programmer error.
inline void require_caps(const IScheduler& s, SchedulerCaps required,
                         const char* context = "scheduler") {
    if (!has_caps(s, required)) {
        throw unsupported_capability(std::string{context}
            + ": required SchedulerCaps not advertised by this scheduler");
    }
}

}  // namespace aria

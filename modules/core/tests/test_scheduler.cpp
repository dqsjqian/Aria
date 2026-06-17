// test_scheduler.cpp
//
// Capability-bitmask contract tests for the unified `aria::IScheduler`
// base class introduced in Sprint3-#2.
//
// What we verify:
//
//   * Bitmask algebra (|, &, |=, has_any, has_all) works as documented.
//   * `IScheduler::schedule_after` default impl throws
//     `unsupported_capability` when not overridden.
//   * Built-in concrete schedulers advertise the capabilities they
//     promise to uphold:
//       - InlineExecutor       : Post + GraphSafe + WorkerSafe
//       - ThreadPoolExecutor   : Post + WorkerSafe + Autonomous
//                                (deliberately no GraphSafe)
//       - MainThreadExecutor   : Post + GraphSafe + WorkerSafe +
//                                MainThread + Pumpable
//       - VirtualTimeExecutor  : Post + Delay + GraphSafe +
//                                WorkerSafe + Pumpable
//       - SimpleDispatcher     : Post + Delay + MainThread + Pumpable
//   * `require_caps` throws cleanly when the requested capability is
//     missing; `has_caps` returns the correct boolean.
//   * Multi-role implementations (e.g. VirtualTimeExecutor inheriting
//     both IExecutor and IDelayedScheduler) collapse to a single
//     IScheduler subobject — no diamond, no ambiguous override.

#include "aria/scheduler.hpp"
#include "aria/async/executor.hpp"
#include "aria/async/virtual_time_executor.hpp"
#include "aria/runtime/dispatcher.hpp"

#include <doctest/doctest.h>

using aria::IScheduler;
using aria::SchedulerCaps;
using aria::has_caps;
using aria::has_all;
using aria::has_any;
using aria::require_caps;
using aria::unsupported_capability;

TEST_CASE("SchedulerCaps: bitmask algebra is total and consistent") {
    constexpr auto a = SchedulerCaps::Post | SchedulerCaps::Delay;
    constexpr auto b = SchedulerCaps::Delay | SchedulerCaps::MainThread;

    static_assert((a & b) == SchedulerCaps::Delay);
    static_assert(has_all(a, SchedulerCaps::Post));
    static_assert(!has_all(a, SchedulerCaps::MainThread));
    static_assert(has_any(a, SchedulerCaps::Delay));
    static_assert(!has_any(SchedulerCaps::Post, SchedulerCaps::Delay));

    SchedulerCaps mut = SchedulerCaps::Post;
    mut |= SchedulerCaps::Delay;
    CHECK(has_all(mut, SchedulerCaps::Post | SchedulerCaps::Delay));
    mut &= SchedulerCaps::Delay;
    CHECK(mut == SchedulerCaps::Delay);
}

namespace {

/// Minimal IScheduler that does NOT advertise Delay — used to verify
/// the default `schedule_after` throws `unsupported_capability`.
class PostOnlyScheduler final : public IScheduler {
public:
    [[nodiscard]] SchedulerCaps caps() const noexcept override {
        return SchedulerCaps::Post;
    }
    void schedule(std::function<void()> fn) override { fn(); }
};

}  // namespace

TEST_CASE("IScheduler: schedule_after default impl throws unsupported_capability") {
    PostOnlyScheduler s;
    CHECK_FALSE(has_caps(s, SchedulerCaps::Delay));
    CHECK_THROWS_AS(s.schedule_after(std::chrono::milliseconds{10}, []{}),
                    unsupported_capability);
}

TEST_CASE("IScheduler: require_caps throws on missing capability, returns silently otherwise") {
    PostOnlyScheduler s;
    CHECK_NOTHROW(require_caps(s, SchedulerCaps::Post, "post-only"));
    CHECK_THROWS_AS(require_caps(s, SchedulerCaps::Delay, "post-only"),
                    unsupported_capability);
    CHECK_THROWS_AS(require_caps(s, SchedulerCaps::Post | SchedulerCaps::Delay,
                                 "post-only"),
                    unsupported_capability);
}

TEST_CASE("IExecutor concrete classes advertise the documented capability set") {
    aria::async::InlineExecutor inline_exec;
    CHECK(inline_exec.caps()
          == (SchedulerCaps::Post | SchedulerCaps::GraphSafe
              | SchedulerCaps::WorkerSafe));
    CHECK(inline_exec.is_safe_graph_executor());
    CHECK(inline_exec.is_safe_worker_executor());

    aria::async::ThreadPoolExecutor pool{1};
    CHECK(pool.caps()
          == (SchedulerCaps::Post | SchedulerCaps::WorkerSafe
              | SchedulerCaps::Autonomous));
    CHECK_FALSE(has_caps(pool, SchedulerCaps::GraphSafe));
    CHECK(has_caps(pool, SchedulerCaps::WorkerSafe));
    // Legacy bool API agrees with caps() bitmask.
    CHECK_FALSE(pool.is_safe_graph_executor());
    CHECK(pool.is_safe_worker_executor());

    aria::async::MainThreadExecutor main_exec;
    CHECK(main_exec.caps()
          == (SchedulerCaps::Post | SchedulerCaps::GraphSafe
              | SchedulerCaps::WorkerSafe | SchedulerCaps::MainThread
              | SchedulerCaps::Pumpable));
    CHECK(main_exec.is_safe_graph_executor());
    CHECK(main_exec.is_main_thread());  // owner not yet bound -> permissive
}

TEST_CASE("VirtualTimeExecutor: multi-role inheritance produces one IScheduler subobject") {
    aria::async::VirtualTimeExecutor vt;

    // Capability bitmask is the explicit override (not the IExecutor or
    // IDelayedScheduler default).
    CHECK(vt.caps()
          == (SchedulerCaps::Post | SchedulerCaps::Delay
              | SchedulerCaps::GraphSafe | SchedulerCaps::WorkerSafe
              | SchedulerCaps::Pumpable));

    // Up-cast through both legs of the diamond — must arrive at the same
    // IScheduler instance thanks to virtual inheritance.
    auto& as_exec  = static_cast<aria::async::IExecutor&>(vt);
    auto& as_delay = static_cast<aria::IDelayedScheduler&>(vt);
    auto* via_exec  = static_cast<IScheduler*>(&as_exec);
    auto* via_delay = static_cast<IScheduler*>(&as_delay);
    CHECK(via_exec == via_delay);

    // Unified entry points work and respect virtual time.
    bool fired_now    = false;
    bool fired_after  = false;
    via_exec->schedule([&]{ fired_now = true; });
    via_exec->schedule_after(std::chrono::milliseconds{50},
                             [&]{ fired_after = true; });
    CHECK_FALSE(fired_now);
    CHECK_FALSE(fired_after);

    vt.advance_by(std::chrono::milliseconds{0});
    CHECK(fired_now);
    CHECK_FALSE(fired_after);

    vt.advance_by(std::chrono::milliseconds{50});
    CHECK(fired_after);
}

TEST_CASE("SimpleDispatcher: caps reflect Post + Delay + MainThread + Pumpable") {
    aria::runtime::SimpleDispatcher d;
    CHECK(d.caps()
          == (SchedulerCaps::Post | SchedulerCaps::Delay
              | SchedulerCaps::MainThread | SchedulerCaps::Pumpable));
    CHECK(has_caps(d, SchedulerCaps::Delay));
    CHECK(d.is_main_thread());

    // Reachable as IScheduler — both legacy and unified entry points
    // route to the same queue.
    IScheduler& s = d;
    bool ran_legacy  = false;
    bool ran_unified = false;
    d.post([&]{ ran_legacy = true; });
    s.schedule([&]{ ran_unified = true; });
    CHECK_FALSE(ran_legacy);
    CHECK_FALSE(ran_unified);

    d.pump(std::chrono::milliseconds{1});
    CHECK(ran_legacy);
    CHECK(ran_unified);
}

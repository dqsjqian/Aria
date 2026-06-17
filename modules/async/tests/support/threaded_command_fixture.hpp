#pragma once

// ThreadedCommandFixture — boilerplate for AsyncCommand tests that mix
// a real ThreadPoolExecutor worker with a MainThreadExecutor graph
// executor. Encapsulates pump/wait helpers so each test reads cleanly:
//
//   ThreadedCommandFixture<int, int> fx{2,
//       [&](CancellationToken tok, int x) -> Task<int> {
//           // ... action body ...
//           co_return x * 10;
//       },
//       AsyncCommandPolicy::LatestOnly};
//
//   fx.cmd.execute(7);
//   REQUIRE(fx.wait_for_idle());
//   CHECK(*fx.cmd.last_result.get() == 70);

#include "aria/async/async_command.hpp"
#include "aria/async/cancellation.hpp"
#include "aria/async/executor.hpp"
#include "aria/async/task.hpp"

#include <chrono>
#include <utility>

namespace aria::async::testing {

/// RAII bundle: graph-thread `ui`, thread-pool `worker`, and a fully
/// wired `AsyncCommand<R, Args...>`. The fixture's destructor cancels
/// the command before the pool tears down, so worker tasks unwind
/// cleanly via `OperationCancelled`.
template<typename R, typename... Args>
class ThreadedCommandFixture {
public:
    MainThreadExecutor  ui;
    ThreadPoolExecutor  worker;
    AsyncCommand<R, Args...> cmd;

    template<typename Fn>
    ThreadedCommandFixture(std::size_t worker_threads,
                           Fn action,
                           AsyncCommandPolicy policy = AsyncCommandPolicy::Parallel)
        : ui{},
          worker{worker_threads},
          cmd{ui, worker, std::move(action), policy} {}

    /// Pump until the command's `is_executing` flips back to false.
    /// Returns true if observed within `timeout`.
    bool wait_for_idle(std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
        return ui.pump_until([this]{ return !cmd.is_executing.get(); }, timeout);
    }

    /// Pump until `predicate()` returns true.
    template<typename Pred>
    bool wait_for(Pred predicate,
                  std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
        return ui.pump_until(std::move(predicate), timeout);
    }
};

}  // namespace aria::async::testing

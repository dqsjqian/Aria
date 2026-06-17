// bench_async_command — AsyncCommand / Task / executor scheduling cost.
//
// Note: numbers here include thread-pool dispatch and graph-thread
// scheduling, so single-digit microseconds per execute is normal even
// in -O3. The point is to detect regressions, not to claim the work
// itself is microsecond-class.

#include "aria/aria.hpp"
#include "aria/abi/version.hpp"
#include "aria/async/async_command.hpp"
#include "aria/async/cancellation.hpp"
#include "aria/async/executor.hpp"
#include "aria/async/task.hpp"

#include "bench_common.hpp"

using namespace aria;
using namespace aria::async;
using namespace aria_bench;

int main() {
    banner(std::string("aria v") + abi::version_string + " — bench_async_command");

    MainThreadExecutor ui;
    ThreadPoolExecutor worker(2);

    // AsyncCommand round-trip (execute + pump until idle)
    {
        AsyncCommand<int, int> cmd(ui, worker,
            [](int x) -> Task<int> {
                co_return x * 2;
            });

        const int N = 5'000;
        double ns = measure_ns(N, [&](int i) {
            cmd.execute(i);
            ui.pump_until([&]{ return !cmd.is_executing.get(); },
                          std::chrono::milliseconds{500});
        });
        row("AsyncCommand<int,int>::execute round-trip", ns, N);
    }

    // AsyncCommand with cancellation token (cooperative)
    {
        AsyncCommand<int, int> cmd(ui, worker,
            [](CancellationToken tok, int x) -> Task<int> {
                tok.throw_if_cancelled();
                co_return x + 1;
            });

        const int N = 5'000;
        double ns = measure_ns(N, [&](int i) {
            cmd.execute(i);
            ui.pump_until([&]{ return !cmd.is_executing.get(); },
                          std::chrono::milliseconds{500});
        });
        row("AsyncCommand<int,int> + CancellationToken", ns, N);
    }

    // CancellationSource flip + token check
    {
        const int N = 1'000'000;
        double ns = measure_ns(N, [&](int) {
            CancellationSource src;
            auto tok = src.token();
            src.cancel();
            (void)tok.is_cancelled();
        });
        row("CancellationSource: cancel + read token", ns, N);
    }

    // -------------------------------------------------------------------
    //  Percentile snapshot — feeds nightly check-bench.sh thresholds.
    //  AsyncCommand round-trip is the canonical cross-thread hot path:
    //  any regression in executor wake-up, graph dispatch, or coroutine
    //  resume cost shows up as a P99 spike here long before it moves
    //  the mean noticeably.
    // -------------------------------------------------------------------
    {
        AsyncCommand<int, int> cmd(ui, worker,
            [](int x) -> Task<int> {
                co_return x * 2;
            });

        constexpr int kSamples      = 64;
        constexpr int kOpsPerSample = 50;
        auto stats = measure_percentiles(kSamples, kOpsPerSample,
            [&](int i) {
                cmd.execute(i);
                ui.pump_until([&]{ return !cmd.is_executing.get(); },
                              std::chrono::milliseconds{500});
            });
        row_pct("AsyncCommand<int,int>::execute round-trip", stats);
    }

    std::cout << "\n=== done ===\n";
    return 0;
}

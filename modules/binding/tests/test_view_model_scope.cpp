#include <doctest/doctest.h>

#include "aria/binding/view_model.hpp"
#include "aria/binding/view_model_scope.hpp"
#include "aria/async/cancellation.hpp"
#include "aria/async/virtual_time_executor.hpp"

using namespace aria;
using namespace aria::async;
using namespace aria::binding;
using namespace std::chrono_literals;

namespace {

// Free-function coroutine bodies (lambda captures + coroutines = dangling).
Task<void> poll_loop(CancellationToken tok,
                     VirtualTimeExecutor& vt,
                     int& tick_count) {
    while (!tok.is_cancelled()) {
        co_await schedule_after(vt, 100ms);
        tok.throw_if_cancelled();
        ++tick_count;
    }
}

}  // namespace

TEST_CASE("CancellationSource: cancel propagates through token") {
    CancellationSource src;
    auto t = src.token();
    CHECK_FALSE(t.is_cancelled());
    src.cancel();
    CHECK(t.is_cancelled());
    CHECK_THROWS_AS(t.throw_if_cancelled(), OperationCancelled);
}

TEST_CASE("CancellationSource: callbacks fire on cancel") {
    CancellationSource src;
    auto t = src.token();

    int hits = 0;
    t.on_cancel([&] { ++hits; });
    t.on_cancel([&] { hits += 10; });

    CHECK(hits == 0);
    src.cancel();
    CHECK(hits == 11);

    // Late subscription on already-cancelled token fires immediately.
    int late = 0;
    t.on_cancel([&] { ++late; });
    CHECK(late == 1);
}

TEST_CASE("CancellationSource auto-cancels on destruction") {
    CancellationToken tok;
    {
        CancellationSource src;
        tok = src.token();
        CHECK_FALSE(tok.is_cancelled());
    }
    CHECK(tok.is_cancelled());
}

TEST_CASE("CoroutineScope cancels in-flight coroutines on destroy") {
    VirtualTimeExecutor vt;
    int ticks = 0;
    {
        CoroutineScope scope;
        scope.launch([&vt, &ticks](CancellationToken tok) {
            return poll_loop(tok, vt, ticks);
        });

        vt.advance_by(100ms);  // tick 1
        vt.advance_by(100ms);  // tick 2
        vt.advance_by(100ms);  // tick 3
        CHECK(ticks == 3);
    }   // <-- scope dtor cancels

    // Even if more virtual time advances, no new ticks.
    vt.advance_by(500ms);
    CHECK(ticks == 3);
}

TEST_CASE("ViewModelScope: VM destruction cancels coroutines") {
    VirtualTimeExecutor vt;
    int ticks = 0;

    struct PollerVm : ViewModel {
        ViewModelScope scope;
        PollerVm() { scope.attach(*this); }
    };

    {
        auto vm = std::make_shared<PollerVm>();
        vm->scope.launch([&vt, &ticks](CancellationToken tok) {
            return poll_loop(tok, vt, ticks);
        });

        vt.advance_by(100ms);
        vt.advance_by(100ms);
        CHECK(ticks == 2);
    }   // vm dtor → add_destroy_hook fires → scope.cancel()

    vt.advance_by(500ms);
    CHECK(ticks == 2);
}

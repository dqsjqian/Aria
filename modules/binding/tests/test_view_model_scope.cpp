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

// Cancellation-aware suspension proves teardown drains before returning.
Task<void> wait_for_scope_cancel(CancellationToken tok, bool& exited) {
    co_await tok;
    exited = true;
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
    bool exited = false;
    {
        CoroutineScope scope;
        scope.launch([&](CancellationToken tok) { return wait_for_scope_cancel(tok, exited); });
        CHECK_FALSE(exited);
    }
    CHECK(exited);
}

TEST_CASE("ViewModelScope: VM destruction cancels coroutines") {
    bool exited = false;
    struct PollerVm : ViewModel {
        ViewModelScope scope;
        PollerVm() { scope.attach(*this); }
    };
    {
        auto vm = std::make_shared<PollerVm>();
        vm->scope.launch([&](CancellationToken tok) { return wait_for_scope_cancel(tok, exited); });
        CHECK_FALSE(exited);
    }
    CHECK(exited);
}

TEST_CASE("ViewModelScope: cancellation precedes earlier declared VM members") {
    bool resource_destroyed = false;
    bool cancelled_while_alive = false;
    struct Resource {
        bool& destroyed;
        ~Resource() { destroyed = true; }
    };
    struct ScopedVm : ViewModel {
        Resource resource;
        ViewModelScope scope; // Declared last, therefore cancelled first.
        explicit ScopedVm(bool& destroyed) : resource{destroyed} { scope.attach(*this); }
    };
    {
        ScopedVm vm(resource_destroyed);
        vm.scope.token().on_cancel([&] { cancelled_while_alive = !resource_destroyed; });
    }
    CHECK(resource_destroyed);
    CHECK(cancelled_while_alive);
}

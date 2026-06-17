#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "aria/abi/signal.hpp"
#include "aria/abi/version.hpp"
#include <atomic>
#include <thread>
#include <vector>

using namespace aria::abi;

namespace {

struct CounterState {
    std::atomic<int>* counter;
};

void counter_invoker(void* state, void* args) noexcept {
    auto* s = static_cast<CounterState*>(state);
    s->counter->fetch_add(1, std::memory_order_relaxed);
    (void)args;
}

void counter_destroyer(void* state) noexcept {
    delete static_cast<CounterState*>(state);
}

SlotErased make_counter_slot(std::atomic<int>& c) {
    auto* state = new CounterState{&c};
    return SlotErased{counter_invoker, counter_destroyer, state};
}

}  // namespace

TEST_CASE("Signal: connect + emit invokes slot") {
    SignalErased sig;
    std::atomic<int> count{0};
    auto id = sig.connect(make_counter_slot(count));
    CHECK(id.valid());
    CHECK(sig.slot_count() == 1);

    sig.emit(nullptr);
    CHECK(count.load() == 1);
}

TEST_CASE("Signal: multiple slots all fire") {
    SignalErased sig;
    std::atomic<int> a{0}, b{0}, c{0};
    sig.connect(make_counter_slot(a));
    sig.connect(make_counter_slot(b));
    sig.connect(make_counter_slot(c));
    CHECK(sig.slot_count() == 3);

    sig.emit(nullptr);
    CHECK(a.load() == 1);
    CHECK(b.load() == 1);
    CHECK(c.load() == 1);
}

TEST_CASE("Signal: disconnect stops invocation") {
    SignalErased sig;
    std::atomic<int> count{0};
    auto id = sig.connect(make_counter_slot(count));

    sig.emit(nullptr);
    CHECK(count.load() == 1);

    sig.disconnect(id);
    CHECK(sig.slot_count() == 0);
    sig.emit(nullptr);
    CHECK(count.load() == 1);  // unchanged
}

TEST_CASE("Signal: clear() removes all slots") {
    SignalErased sig;
    std::atomic<int> a{0}, b{0};
    sig.connect(make_counter_slot(a));
    sig.connect(make_counter_slot(b));
    sig.clear();
    CHECK(sig.slot_count() == 0);

    sig.emit(nullptr);
    CHECK(a.load() == 0);
    CHECK(b.load() == 0);
}

TEST_CASE("Signal: weak handle survives signal destruction") {
    std::weak_ptr<SignalErased::ControlBlock> weak;
    SlotId id;
    {
        SignalErased sig;
        std::atomic<int> count{0};
        id = sig.connect(make_counter_slot(count));
        weak = sig.weak_handle();
        // sig goes out of scope here
    }
    // disconnect after destruction must not crash
    SignalErased::disconnect_via_weak(weak, id);
    CHECK(weak.expired());
}

TEST_CASE("Signal: concurrent emits are thread-safe") {
    SignalErased sig;
    std::atomic<int> count{0};
    sig.connect(make_counter_slot(count));

    constexpr int kThreads = 8;
    constexpr int kIters = 1000;
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < kIters; ++j) sig.emit(nullptr);
        });
    }
    for (auto& t : threads) t.join();
    CHECK(count.load() == kThreads * kIters);
}

// ── B6 regression: every method on a moved-from SignalErased must be
// a safe no-op rather than dereferencing a null Impl pointer. The
// classic C++ contract says moved-from objects are destructible-only,
// but in practice users sometimes hand them to other code (containers,
// pimpl wrappers, ABI bridges) that calls observers / mutators on
// them. The pre-fix implementation would segfault inside `connect`,
// `emit`, `slot_count`, `clear`, `disconnect`, and `weak_handle`.
TEST_CASE("Signal: moved-from instance is a safe no-op (B6)") {
    SignalErased original;
    std::atomic<int> count{0};
    original.connect(make_counter_slot(count));
    CHECK(original.slot_count() == 1);

    SignalErased moved_to{std::move(original)};
    // `original` is now in the moved-from state. Every public surface
    // below used to crash; now it must degrade gracefully.
    CHECK(original.slot_count() == 0);
    CHECK(original.weak_handle().expired());
    original.emit(nullptr);                                  // no crash, no fire
    SlotId id = original.connect(make_counter_slot(count));  // returns invalid id
    CHECK_FALSE(id.valid());
    original.disconnect(SlotId{42});                          // no-op
    original.clear();                                         // no-op

    // The destination still works normally.
    moved_to.emit(nullptr);
    CHECK(count.load() == 1);
}

TEST_CASE("Version constants are sane") {
    CHECK(version_major == 1);
    CHECK(abi_version == 1);
}

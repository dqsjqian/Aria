// ============================================================================
//  test_callback_boundary_integration.cpp
// ----------------------------------------------------------------------------
//  End-to-end coverage of the unified callback-boundary plumbing across the
//  five framework-internal noexcept boundaries:
//    1. ThreadPoolExecutor worker
//    2. MainThreadExecutor drain
//    3. MainThreadExecutor run_one
//    4. SimpleDispatcher pump
//    5. ABI SlotErased trampoline
//
//  The runtime bootstrap also gets exercised end-to-end: install →
//  exercise → uninstall → confirm reversion.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/abi/signal.hpp"
#include "aria/abi/slot.hpp"
#include "aria/abi/slot_factory.hpp"
#include "aria/async/executor.hpp"
#include "aria/callback_boundary.hpp"
#include "aria/runtime/dispatcher.hpp"
#include "aria/runtime/runtime_bootstrap.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

// Capture every CallbackFailure routed through the global sink. Keep
// state process-global so the function-pointer sink can find it.
struct Capture {
    std::mutex                mu;
    std::vector<std::string>  categories;
    std::vector<std::string>  messages;
    std::atomic<int>          calls{0};

    void reset() noexcept {
        std::lock_guard lk(mu);
        categories.clear();
        messages.clear();
        calls.store(0, std::memory_order_release);
    }

    [[nodiscard]] bool saw(std::string_view category) {
        std::lock_guard lk(mu);
        for (auto& c : categories) {
            if (c == category) return true;
        }
        return false;
    }
};

inline Capture& capture() {
    static Capture c;
    return c;
}

void capture_sink(const aria::CallbackFailure& f) {
    auto& c = capture();
    std::lock_guard lk(c.mu);
    c.categories.emplace_back(f.category);
    if (f.message.empty() && f.exception) {
        try {
            std::rethrow_exception(f.exception);
        } catch (const std::exception& e) {
            c.messages.emplace_back(e.what());
        } catch (...) {
            c.messages.emplace_back("non-std");
        }
    } else {
        c.messages.emplace_back(f.message);
    }
    c.calls.fetch_add(1, std::memory_order_acq_rel);
}

class SinkScope {
public:
    SinkScope() noexcept
        : previous_(aria::set_callback_failure_sink(&capture_sink)) {
        capture().reset();
    }
    ~SinkScope() noexcept {
        aria::set_callback_failure_sink(previous_);
    }
    SinkScope(const SinkScope&)            = delete;
    SinkScope& operator=(const SinkScope&) = delete;

private:
    aria::CallbackFailureSink previous_;
};

// Spin until predicate is true or the deadline passes.
template <class Pred>
bool wait_until(Pred p, std::chrono::milliseconds budget = std::chrono::milliseconds{1000}) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!p()) {
        if (std::chrono::steady_clock::now() > deadline) return p();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return true;
}

}  // namespace

// ----------------------------------------------------------------------------
//  CBI-1: ThreadPoolExecutor worker reports through the unified sink.
// ----------------------------------------------------------------------------
TEST_CASE("callback_boundary_integration: ThreadPoolExecutor worker") {
    SinkScope scope;
    aria::async::ThreadPoolExecutor pool{2};
    pool.post([] { throw std::runtime_error{"worker-boom"}; });

    REQUIRE(wait_until([&] {
        return capture().saw(std::string_view{"executor.thread_pool.worker"});
    }));
    // Pool drains/joins via its destructor when the test scope exits.
}

// ----------------------------------------------------------------------------
//  CBI-2: MainThreadExecutor drain() reports via the unified sink.
// ----------------------------------------------------------------------------
TEST_CASE("callback_boundary_integration: MainThreadExecutor drain") {
    SinkScope scope;
    aria::async::MainThreadExecutor mt;
    mt.post([] { throw std::runtime_error{"drain-boom"}; });
    mt.drain();

    CHECK(capture().saw(std::string_view{"executor.main_thread.drain"}));
}

// ----------------------------------------------------------------------------
//  CBI-3: MainThreadExecutor run_one() reports via the unified sink.
// ----------------------------------------------------------------------------
TEST_CASE("callback_boundary_integration: MainThreadExecutor run_one") {
    SinkScope scope;
    aria::async::MainThreadExecutor mt;
    mt.post([] { throw std::runtime_error{"run_one-boom"}; });
    mt.run_one();

    CHECK(capture().saw(std::string_view{"executor.main_thread.run_one"}));
}

// ----------------------------------------------------------------------------
//  CBI-4: SimpleDispatcher pump reports via the unified sink.
// ----------------------------------------------------------------------------
TEST_CASE("callback_boundary_integration: SimpleDispatcher pump") {
    SinkScope scope;
    aria::runtime::SimpleDispatcher disp;
    disp.post([] { throw std::runtime_error{"pump-boom"}; });
    disp.pump();

    CHECK(capture().saw(std::string_view{"runtime.simple_dispatcher.pump"}));
}

// ----------------------------------------------------------------------------
//  CBI-5: ABI slot trampoline reports via the slot-invoke hook (which
//  bootstrap routes into the unified sink).
// ----------------------------------------------------------------------------
TEST_CASE("callback_boundary_integration: ABI slot trampoline via runtime bootstrap") {
    // Bootstrap installs both the failure sink AND the abi slot hook.
    REQUIRE(aria::runtime::install_default_diagnostics());

    // Override the failure sink installed by bootstrap so we can observe
    // the slot.invoke route. The previous sink (logger bridge) is
    // remembered and restored at the end of the test.
    auto* prev_sink = aria::set_callback_failure_sink(&capture_sink);
    capture().reset();

    // Build a typed signal/slot pair where the slot throws.
    using namespace aria::abi;
    SignalErased signal;
    auto slot = make_slot_erased([](void*) {
        throw std::runtime_error{"slot-boom"};
    });
    signal.connect(std::move(slot));
    int payload = 42;
    signal.emit(&payload);

    CHECK(capture().saw(std::string_view{"abi.slot.invoke"}));

    // Restore: revert to whatever bootstrap had installed, then uninstall.
    aria::set_callback_failure_sink(prev_sink);
    aria::runtime::uninstall_default_diagnostics();
}

// ----------------------------------------------------------------------------
//  CBI-6: install_default_diagnostics() is idempotent and reversible.
// ----------------------------------------------------------------------------
TEST_CASE("callback_boundary_integration: bootstrap idempotence and rollback") {
    // First install — must report "installed".
    CHECK(aria::runtime::install_default_diagnostics());
    // Second install — already installed, no-op.
    CHECK_FALSE(aria::runtime::install_default_diagnostics());

    // Uninstall once: reverts.
    aria::runtime::uninstall_default_diagnostics();
    // Uninstall again: safe no-op.
    aria::runtime::uninstall_default_diagnostics();

    // After full rollback the sink should be back to nullptr (stderr
    // fallback) and the abi slot hook to nullptr.
    CHECK(aria::current_callback_failure_sink() == nullptr);
    CHECK(aria::abi::set_slot_invoke_failure_hook(nullptr) == nullptr);
}

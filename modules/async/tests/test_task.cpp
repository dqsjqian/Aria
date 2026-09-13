#include <doctest/doctest.h>

#include "aria/async/task.hpp"
#include "aria/async/cancellation.hpp"
#include "aria/async/scope.hpp"
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

using namespace aria::async;

namespace {

Task<int> make_int(int x) { co_return x * 2; }
Task<void> make_void() { co_return; }

Task<std::string> make_string() {
    auto a = co_await make_int(7);
    co_return std::string("got ") + std::to_string(a);
}

Task<int> throws_one() {
    throw std::runtime_error("boom");
    co_return 0;
}

struct FrameLifetime {
    explicit FrameLifetime(int& count) : destroyed(count) {}
    FrameLifetime(const FrameLifetime&) = delete;
    FrameLifetime& operator=(const FrameLifetime&) = delete;
    FrameLifetime(FrameLifetime&&) = delete;
    FrameLifetime& operator=(FrameLifetime&&) = delete;
    ~FrameLifetime() { ++destroyed; }
    int& destroyed;
};

struct ResumeSlot {
    std::coroutine_handle<> pending;

    [[nodiscard]] static bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) noexcept { pending = h; }
    void await_resume() const noexcept {}

    void resume() {
        auto h = std::exchange(pending, {});
        if (h) {
            h.resume();
        }
    }
};

template<typename T>
Task<T> tracked_task(std::shared_ptr<FrameLifetime> lifetime, ResumeSlot* slot,
                     int& completed, bool fail = false) {
    (void)lifetime; // A parameter remains owned by the frame through final suspend.
    if (slot) {
        co_await *slot;
    }
    ++completed;
    if (fail) {
        throw std::runtime_error("tracked failure");
    }
    if constexpr (std::is_void_v<T>) {
        co_return;
    } else {
        co_return 42;
    }
}

Task<void> tracked_parent(std::shared_ptr<FrameLifetime> lifetime, Task<int> child,
                          int& result) {
    (void)lifetime;
    result = co_await std::move(child);
}

Task<void> tracked_cancellation(std::shared_ptr<FrameLifetime> lifetime,
                                CancellationToken token, bool& resumed) {
    (void)lifetime;
    co_await token;
    resumed = true;
    token.throw_if_cancelled();
}

Task<std::shared_ptr<FrameLifetime>> tracked_result(std::shared_ptr<FrameLifetime> lifetime) {
    co_return lifetime;
}

}  // namespace

TEST_CASE("Task<int>: blocking_get returns value") {
    auto t = make_int(21);
    CHECK(t.blocking_get() == 42);
}

TEST_CASE("Task<void>: blocking_get does not throw") {
    auto t = make_void();
    t.blocking_get();
    CHECK(t.done());
}

TEST_CASE("Task: chained co_await works") {
    auto t = make_string();
    CHECK(t.blocking_get() == "got 14");
}

TEST_CASE("Task: exceptions propagate via blocking_get") {
    auto t = throws_one();
    CHECK_THROWS_AS(t.blocking_get(), std::runtime_error);
}

TEST_CASE_TEMPLATE("Task: synchronous detach releases the frame exactly once", T, void, int) {
    bool fail = false;
    SUBCASE("normal completion") {}
    SUBCASE("unobserved exception") { fail = true; }

    int destroyed = 0;
    int completed = 0;
    auto lifetime = std::make_shared<FrameLifetime>(destroyed);
    std::weak_ptr<FrameLifetime> weak = lifetime;
    auto task = tracked_task<T>(std::move(lifetime), nullptr, completed, fail);
    CHECK_NOTHROW(std::move(task).start_detached());
    CHECK(completed == 1);
    CHECK(destroyed == 1);
    CHECK(weak.expired());
    CHECK_FALSE(task.done());
}

TEST_CASE_TEMPLATE("Task: detached suspension outlives its original Task owner", T, void, int) {
    int destroyed = 0;
    int completed = 0;
    ResumeSlot slot;
    auto lifetime = std::make_shared<FrameLifetime>(destroyed);
    std::weak_ptr<FrameLifetime> weak = lifetime;
    {
        auto task = tracked_task<T>(std::move(lifetime), &slot, completed);
        std::move(task).start_detached();
        CHECK_FALSE(task.done());
    }
    REQUIRE(static_cast<bool>(slot.pending));
    CHECK(completed == 0);
    CHECK(destroyed == 0);
    CHECK_FALSE(weak.expired());

    slot.resume();

    CHECK_FALSE(slot.pending);
    CHECK(completed == 1);
    CHECK(destroyed == 1);
    CHECK(weak.expired());
}

TEST_CASE_TEMPLATE("Task: attached completion retains frame until its owner releases it", T, void, int) {
    bool fail = false;
    SUBCASE("value remains observable") {}
    SUBCASE("exception remains observable") { fail = true; }

    int destroyed = 0;
    int completed = 0;
    auto lifetime = std::make_shared<FrameLifetime>(destroyed);
    std::weak_ptr<FrameLifetime> weak = lifetime;
    {
        auto task = tracked_task<T>(std::move(lifetime), nullptr, completed, fail);
        task.start();
        REQUIRE(task.done());
        CHECK(completed == 1);
        CHECK(destroyed == 0);
        CHECK_FALSE(weak.expired());
        if (fail) {
            CHECK_THROWS_WITH_AS(task.blocking_get(), "tracked failure", std::runtime_error);
        } else if constexpr (std::is_void_v<T>) {
            CHECK_NOTHROW(task.blocking_get());
        } else {
            CHECK(task.blocking_get() == 42);
        }
    }
    CHECK(destroyed == 1);
    CHECK(weak.expired());
}

TEST_CASE_TEMPLATE("Task: detaching a completed task destroys without resuming it", T, void, int) {
    int destroyed = 0;
    int completed = 0;
    auto lifetime = std::make_shared<FrameLifetime>(destroyed);
    std::weak_ptr<FrameLifetime> weak = lifetime;
    auto task = tracked_task<T>(std::move(lifetime), nullptr, completed);
    task.start();
    REQUIRE(task.done());
    CHECK(destroyed == 0);

    std::move(task).start_detached();

    CHECK_FALSE(task.done());
    CHECK(completed == 1);
    CHECK(destroyed == 1);
    CHECK(weak.expired());
    CHECK_NOTHROW(std::move(task).start_detached()); // Empty detached owner is harmless.
}

TEST_CASE("Task: detached parent releases an awaited child after symmetric transfer") {
    int parent_destroyed = 0;
    int child_destroyed = 0;
    int child_completed = 0;
    int result = 0;
    ResumeSlot slot;
    auto parent = std::make_shared<FrameLifetime>(parent_destroyed);
    auto child = std::make_shared<FrameLifetime>(child_destroyed);
    std::weak_ptr<FrameLifetime> parent_weak = parent;
    std::weak_ptr<FrameLifetime> child_weak = child;
    tracked_parent(std::move(parent), tracked_task<int>(std::move(child), &slot, child_completed),
                   result).start_detached();
    REQUIRE(static_cast<bool>(slot.pending));
    CHECK(parent_destroyed == 0);
    CHECK(child_destroyed == 0);

    slot.resume();

    CHECK(result == 42);
    CHECK(child_completed == 1);
    CHECK(parent_destroyed == 1);
    CHECK(child_destroyed == 1);
    CHECK(parent_weak.expired());
    CHECK(child_weak.expired());
}

TEST_CASE("Task: cancellation releases a detached frame after resumption") {
    int destroyed = 0;
    bool resumed = false;
    CancellationSource source;
    auto lifetime = std::make_shared<FrameLifetime>(destroyed);
    std::weak_ptr<FrameLifetime> weak = lifetime;
    tracked_cancellation(std::move(lifetime), source.token(), resumed).start_detached();
    CHECK_FALSE(resumed);
    CHECK(destroyed == 0);

    CHECK_NOTHROW(source.cancel());

    CHECK(resumed);
    CHECK(destroyed == 1);
    CHECK(weak.expired());
}

TEST_CASE("Task: detach releases an unobserved owning result") {
    int destroyed = 0;
    auto lifetime = std::make_shared<FrameLifetime>(destroyed);
    std::weak_ptr<FrameLifetime> weak = lifetime;
    tracked_result(std::move(lifetime)).start_detached();
    CHECK(destroyed == 1);
    CHECK(weak.expired());
}

#include <doctest/doctest.h>

#include "aria/adapters/qt6/qt_dispatcher.hpp"

#include <QEvent>

#include <atomic>
#include <chrono>
#include <climits>
#include <memory>
#include <stdexcept>
#include <thread>

using aria::adapters::qt6::QtDispatcher;
using namespace std::chrono_literals;

namespace {
// Give optimizing compilers the heap parent's exact dynamic type.
class TestParent final : public QObject {};

void pump_qt_dispatcher() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

int dispatcher_failures = 0;
void dispatcher_failure_sink(const aria::CallbackFailure& failure) {
    CHECK(failure.category == "qt.dispatcher");
    CHECK(failure.exception != nullptr);
    ++dispatcher_failures;
}

struct ReentrantPost {
    QtDispatcher* dispatcher;
    int* releases;
    ~ReentrantPost() {
        ++*releases;
        dispatcher->post([] {});
    }
};
} // namespace

TEST_CASE("QtDispatcher posts asynchronously on its context thread") {
    QObject context;
    QtDispatcher dispatcher(&context);
    auto* owner = QThread::currentThread();
    int calls = 0;
    dispatcher.post([&] { CHECK(QThread::currentThread() == owner); ++calls; });
    std::thread producer([&] {
        CHECK_FALSE(dispatcher.is_main_thread());
        dispatcher.post([&] { CHECK(QThread::currentThread() == owner); ++calls; });
    });
    producer.join();
    CHECK(calls == 0);
    CHECK(dispatcher.is_main_thread());
    CHECK(dispatcher.caps() == (aria::SchedulerCaps::Post | aria::SchedulerCaps::Delay |
                               aria::SchedulerCaps::MainThread | aria::SchedulerCaps::Autonomous));
    pump_qt_dispatcher();
    CHECK(calls == 2);
}

TEST_CASE("QtDispatcher validates context ownership") {
    CHECK_THROWS_AS(QtDispatcher(nullptr), std::invalid_argument);
    QObject context;
    std::thread worker([&] {
        CHECK_THROWS_AS(QtDispatcher{&context}, std::logic_error);
    });
    worker.join();
}

TEST_CASE("QtDispatcher contains callback exceptions and ignores empty callbacks") {
    QObject context;
    QtDispatcher dispatcher(&context);
    dispatcher_failures = 0;
    const auto previous = aria::set_callback_failure_sink(dispatcher_failure_sink);
    dispatcher.post({});
    dispatcher.post_delayed(1ms, {});
    dispatcher.post([] { throw std::runtime_error("posted failure"); });
    dispatcher.post_delayed(1ms, [] { throw std::runtime_error("delayed failure"); });
    std::this_thread::sleep_for(2ms);
    CHECK_NOTHROW(pump_qt_dispatcher());
    aria::set_callback_failure_sink(previous);
    CHECK(dispatcher_failures == 2);
}

TEST_CASE("QtDispatcher releases pending captures synchronously on shutdown") {
    QObject context;
    auto dispatcher = std::make_unique<QtDispatcher>(&context);
    int calls = 0;
    auto capture = std::make_shared<int>(1);
    dispatcher->post([&, capture] { ++calls; });
    dispatcher->post_delayed(1h, [&, capture] { ++calls; });
    CHECK(capture.use_count() == 3);
    dispatcher.reset();
    CHECK(capture.use_count() == 1);
    pump_qt_dispatcher();
    CHECK(calls == 0);
}

TEST_CASE("QtDispatcher cancels armed delayed tasks without waiting for timer deletion") {
    QObject context;
    auto dispatcher = std::make_unique<QtDispatcher>(&context);
    auto capture = std::make_shared<int>(1);
    dispatcher->post_delayed(1h, [capture] {});
    pump_qt_dispatcher();
    REQUIRE(context.findChildren<QTimer*>().size() == 1);
    CHECK(capture.use_count() == 2);
    std::thread shutdown([&] { dispatcher.reset(); });
    shutdown.join();
    CHECK(capture.use_count() == 1);
    pump_qt_dispatcher();
    CHECK(context.findChildren<QTimer*>().empty());
}

TEST_CASE("QtDispatcher callback can destroy dispatcher and stop the pending batch") {
    QObject context;
    auto dispatcher = std::make_unique<QtDispatcher>(&context);
    int calls = 0;
    dispatcher->post([&] { ++calls; dispatcher.reset(); });
    dispatcher->post([&] { calls += 10; });
    pump_qt_dispatcher();
    CHECK(calls == 1);
}

TEST_CASE("QtDispatcher shutdown releases reentrant captures outside the state mutex") {
    QObject context;
    auto dispatcher = std::make_unique<QtDispatcher>(&context);
    int releases = 0;
    auto capture = std::shared_ptr<ReentrantPost>(new ReentrantPost{dispatcher.get(), &releases});
    dispatcher->post([capture] {});
    capture.reset();
    dispatcher.reset();
    CHECK(releases == 1);
    pump_qt_dispatcher();
}

TEST_CASE("QtDispatcher survives context destruction and releases captures") {
    auto context = std::make_unique<TestParent>();
    QtDispatcher dispatcher(context.get());
    auto capture = std::make_shared<int>(1);
    dispatcher.post([capture] {});
    context.reset();
    CHECK(capture.use_count() == 1);
    CHECK_FALSE(dispatcher.is_main_thread());
    dispatcher.post([capture] {});
    CHECK(capture.use_count() == 1);
    pump_qt_dispatcher();
}

TEST_CASE("QtDispatcher gates worker posts while the context is destroyed") {
    for (int round = 0; round < 32; ++round) {
        auto context = std::make_unique<TestParent>();
        QtDispatcher dispatcher(context.get());
        std::atomic<bool> started{false};
        std::thread producer([&] {
            started.store(true, std::memory_order_release);
            for (int i = 0; i < 64; ++i) dispatcher.post([] {});
        });
        while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
        context.reset();
        producer.join();
        CHECK_FALSE(dispatcher.is_main_thread());
    }
    pump_qt_dispatcher();
}

TEST_CASE("QtDispatcher chunks extreme delays without narrowing or early delivery") {
    QObject context;
    QtDispatcher dispatcher(&context);
    int delayed_calls = 0;
    int immediate_calls = 0;
    dispatcher.post_delayed(std::chrono::milliseconds::max(), [&] { ++delayed_calls; });
    dispatcher.post_delayed(std::chrono::milliseconds{static_cast<long long>(INT_MAX) + 1},
                            [&] { ++delayed_calls; });
    dispatcher.post_delayed(std::chrono::milliseconds::min(), [&] { ++immediate_calls; });
    pump_qt_dispatcher();
    const auto timers = context.findChildren<QTimer*>();
    REQUIRE(timers.size() == 2);
    for (const auto* timer : timers) CHECK(timer->interval() == INT_MAX);
    std::this_thread::sleep_for(3ms);
    pump_qt_dispatcher();
    CHECK(delayed_calls == 0);
    CHECK(immediate_calls == 1);
}

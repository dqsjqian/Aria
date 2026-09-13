#include <doctest/doctest.h>

#include "aria/runtime/logger.hpp"
#include <stdexcept>
#include <string>
#include <vector>

using namespace aria::runtime;

TEST_CASE("Logger: custom sink receives messages") {
    auto& log = Logger::instance();
    std::vector<std::string> captured;
    log.set_level(LogLevel::Debug);
    log.set_sink([&](LogLevel l, std::string_view cat, std::string_view msg) {
        std::string s = std::string(level_name(l)) + "|" +
                        std::string(cat) + "|" +
                        std::string(msg);
        captured.push_back(s);
    });

    log.info("test", "hello");
    log.warn("test", "watch out");

    CHECK(captured.size() == 2);
    CHECK(captured[0].find("INFO") == 0);
    CHECK(captured[1].find("WARN") == 0);

    log.set_sink(nullptr);  // restore default
}

TEST_CASE("Logger: level filters messages below threshold") {
    auto& log = Logger::instance();
    int n = 0;
    log.set_level(LogLevel::Warn);
    log.set_sink([&](LogLevel, std::string_view, std::string_view) { ++n; });

    log.info("c", "ignored");
    log.debug("c", "ignored");
    log.warn("c", "captured");
    log.error("c", "captured");

    CHECK(n == 2);
    log.set_sink(nullptr);
    log.set_level(LogLevel::Info);
}

TEST_CASE("Logger: level_name strings are stable") {
    CHECK(std::string(level_name(LogLevel::Trace)) == "TRACE");
    CHECK(std::string(level_name(LogLevel::Fatal)) == "FATAL");
}

// ─── Sprint4-#5: Logger never propagates exceptions out of log() ───────────
//
// Contract: if a user-installed sink throws, Logger::log catches the
// exception and falls back to stderr with a "(sink threw: ...)" marker.
// This protects framework-internal noexcept boundaries (executor workers,
// dispatcher pumps, etc.) from propagating sink failures.
TEST_CASE("Logger: throwing sink does not propagate (std::exception)") {
    auto& log = Logger::instance();
    log.set_level(LogLevel::Info);
    log.set_sink([](LogLevel, std::string_view, std::string_view) {
        throw std::runtime_error("sink-boom");
    });

    // Must not throw out of Logger::log even though the sink does.
    CHECK_NOTHROW(log.info("logger.fallback", "trigger"));
    CHECK_NOTHROW(log.error("logger.fallback", "trigger again"));

    log.set_sink(nullptr);  // restore default
}

TEST_CASE("Logger: throwing sink does not propagate (non-std exception)") {
    auto& log = Logger::instance();
    log.set_level(LogLevel::Info);
    log.set_sink([](LogLevel, std::string_view, std::string_view) {
        throw 42;  // non-std exception path
    });

    CHECK_NOTHROW(log.warn("logger.fallback", "non-std payload"));

    log.set_sink(nullptr);  // restore default
}

TEST_CASE("Logger: logging never copies user sink captures") {
    auto& log = Logger::instance();
    struct Sink {
        std::shared_ptr<bool> reject_copy;
        int* calls;
        Sink(std::shared_ptr<bool> reject, int& count)
            : reject_copy(std::move(reject)), calls(&count) {}
        Sink(const Sink& other) : reject_copy(other.reject_copy), calls(other.calls) {
            if (*reject_copy) throw std::runtime_error("unexpected sink copy");
        }
        void operator()(LogLevel, std::string_view, std::string_view) { ++*calls; }
    };
    auto reject_copy = std::make_shared<bool>(false);
    int calls = 0;
    log.set_sink(Sink{reject_copy, calls});
    *reject_copy = true;
    CHECK_NOTHROW(log.info("test", "no capture copy"));
    CHECK(calls == 1);
    log.set_sink(nullptr);
}

TEST_CASE("Logger: sink capture destruction may replace the sink") {
    auto& log = Logger::instance();
    bool released = false;
    auto token = std::shared_ptr<int>(new int, [&](int* value) {
        delete value;
        log.set_sink(nullptr);
        released = true;
    });
    log.set_sink([token = std::move(token)](LogLevel, std::string_view, std::string_view) {});
    log.set_sink(nullptr);
    CHECK(released);
}

TEST_CASE("Logger: recursive sink uses the fallback without recursive invocation") {
    auto& log = Logger::instance();
    int calls = 0;
    log.set_sink([&](LogLevel, std::string_view, std::string_view) {
        ++calls;
        log.info("test", "recursive message");
    });
    log.info("test", "outer message");
    log.set_sink(nullptr);
    CHECK(calls == 1);
}

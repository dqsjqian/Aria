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

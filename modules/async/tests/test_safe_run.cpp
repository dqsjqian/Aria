#include <doctest/doctest.h>

#include "aria/async/executor.hpp"
#include "aria/async/safe_run.hpp"
#include "aria/async/task.hpp"

#include <stdexcept>
#include <string>

using namespace aria::async;

TEST_CASE("on_ui<int>: returns the value") {
    InlineExecutor ui;
    auto t = on_ui(ui, []() -> Task<int> { co_return 42; });
    CHECK(t.blocking_get() == 42);
}

TEST_CASE("on_ui<void>: completes") {
    InlineExecutor ui;
    int side = 0;
    auto t = on_ui(ui, [&]() -> Task<void> { side = 1; co_return; });
    t.blocking_get();
    CHECK(side == 1);
}

TEST_CASE("on_ui: rethrows exceptions") {
    InlineExecutor ui;
    auto t = on_ui(ui, []() -> Task<int> {
        throw std::runtime_error("nope"); co_return 0;
    });
    CHECK_THROWS_AS(t.blocking_get(), std::runtime_error);
}

TEST_CASE("on_ui_safe: routes errors to handler instead of rethrowing") {
    InlineExecutor ui;
    std::string captured;
    auto t = on_ui_safe(ui,
        []() -> Task<void> { throw std::runtime_error("wrong"); co_return; },
        [&](const std::exception& e) { captured = e.what(); }
    );
    t.blocking_get();   // does NOT throw
    CHECK(captured == "wrong");
}

TEST_CASE("on_ui_safe: success path leaves error handler alone") {
    InlineExecutor ui;
    std::string captured = "untouched";
    auto t = on_ui_safe(ui,
        []() -> Task<void> { co_return; },
        [&](const std::exception& e) { captured = e.what(); }
    );
    t.blocking_get();
    CHECK(captured == "untouched");
}

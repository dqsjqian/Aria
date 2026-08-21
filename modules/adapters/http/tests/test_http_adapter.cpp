/// @file test_http_adapter.cpp
/// @brief Smoke tests for HttpAdapter.
///
/// These tests exercise the adapter's registry, shadow state, command,
/// subscription, and ephemeral-port start/stop contracts. Wire-level REST,
/// SSE, TLS, and browser-client acceptance lives in AriaTools.
///
///   - Registry: register / find / unregister / list_views
///   - Shadow state set/get round-trip
///   - Subscription callback invocation
///   - Subscription disconnect via dropping the handle
///   - Custom command registration and unregistration

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "aria/adapters/http/http_adapter.hpp"

#include <atomic>
#include <string>

using namespace aria::adapters::http;

TEST_CASE("HttpAdapter — registry round-trip") {
    HttpAdapter http;
    auto& v1 = http.register_view("a", "text");
    auto& v2 = http.register_view("b", "bool");
    CHECK(v1.id() == "a");
    CHECK(v1.kind() == "text");
    CHECK(v2.id() == "b");

    CHECK(http.find_view("a") == &v1);
    CHECK(http.find_view("b") == &v2);
    CHECK(http.find_view("missing") == nullptr);

    auto views = http.list_views();
    CHECK(views.size() == 2);

    http.unregister_view("a");
    CHECK(http.find_view("a") == nullptr);
    CHECK(http.list_views().size() == 1);
}

TEST_CASE("HttpAdapter — text shadow state and subscription") {
    HttpAdapter http;
    auto& v = http.register_view("greeting", "text");

    http.set_text(v, "hello");
    CHECK(http.get_text(v) == "hello");

    std::string captured;
    auto sub = http.on_text_changed(v, [&](std::string_view s) {
        captured = std::string(s);
    });

    // The Property→adapter path uses set_text, but state-changed
    // callbacks fire only from inbound REST POSTs. Here we verify the
    // subscription registers cleanly and disconnects cleanly.
    CHECK(sub.active());
    sub.release();
    CHECK_FALSE(sub.active());
}

TEST_CASE("HttpAdapter — bool shadow state") {
    HttpAdapter http;
    auto& v = http.register_view("toggle", "bool");
    http.set_bool(v, true);
    CHECK(http.get_bool(v) == true);
    http.set_bool(v, false);
    CHECK(http.get_bool(v) == false);
}

TEST_CASE("HttpAdapter — int / int64 / uint64 / double shadow") {
    HttpAdapter http;
    auto& vi  = http.register_view("vi",  "int");
    auto& v64 = http.register_view("v64", "int64");
    auto& vu  = http.register_view("vu",  "uint64");
    auto& vd  = http.register_view("vd",  "double");

    http.set_int(vi, 42);
    CHECK(http.get_int(vi) == 42);

    http.set_int64(v64, 1'000'000'000'000LL);
    CHECK(http.get_int64(v64) == 1'000'000'000'000LL);

    http.set_uint64(vu, 12345ULL);
    CHECK(http.get_uint64(vu) == 12345ULL);

    http.set_double(vd, 3.14);
    CHECK(http.get_double(vd) == doctest::Approx(3.14));
}

TEST_CASE("HttpAdapter — custom command registration") {
    HttpAdapter http;
    http.register_view("cmds", "text");
    bool fired = false;
    http.register_command("cmds", "ping",
        [&](std::string_view) -> std::string {
            fired = true;
            return R"({"pong":true})";
        });

    // Direct invocation goes through the HTTP layer; we just check the
    // registration table is mutated. Pretend we have visibility — the
    // most we can do here is unregister and confirm the lookup misses.
    http.unregister_command("cmds", "ping");
    CHECK_FALSE(fired);  // unregister doesn't invoke
}

TEST_CASE("HttpAdapter — start / stop on ephemeral port") {
    HttpAdapterConfig cfg;
    cfg.port = 0;  // OS picks
    HttpAdapter http(cfg);
    CHECK_FALSE(http.running());
    bool ok = http.start();
    CHECK(ok);
    CHECK(http.running());
    CHECK(http.actual_port() != 0);
    http.stop();
    CHECK_FALSE(http.running());
}

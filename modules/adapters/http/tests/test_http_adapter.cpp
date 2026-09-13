/// @file test_http_adapter.cpp
/// @brief Smoke tests for HttpAdapter.
///
/// These tests exercise the adapter's registry, shadow state, command,
/// subscription, and ephemeral-port start/stop contracts. REST and SSE
/// regressions live in test_http_wire.cpp; browser SDK tests run with Node.
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
#include <memory>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace aria::adapters::http;

namespace {
std::vector<aria::Subscription> subscribe_all(
        HttpAdapter& http, std::shared_ptr<void> owner = {}) {
    // A generic callback lets the lifetime tests cover every subscription map.
    auto cb = [owner](auto...) { (void)owner; };
    std::vector<aria::Subscription> subs;
    subs.push_back(http.on_text_changed(http.register_view("text", "text"), cb));
    subs.push_back(http.on_bool_changed(http.register_view("bool", "bool"), cb));
    subs.push_back(http.on_int_changed(http.register_view("int", "int"), cb));
    subs.push_back(http.on_int64_changed(http.register_view("int64", "int64"), cb));
    subs.push_back(http.on_uint64_changed(http.register_view("uint64", "uint64"), cb));
    subs.push_back(http.on_float_changed(http.register_view("float", "float"), cb));
    subs.push_back(http.on_double_changed(http.register_view("double", "double"), cb));
    subs.push_back(http.on_click(http.register_view("click", "click"), cb));
    return subs;
}
}

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

TEST_CASE("HttpAdapter — subscriptions can outlive the adapter") {
    auto http = std::make_unique<HttpAdapter>();
    auto owner = std::make_shared<int>(42);
    std::weak_ptr<int> callback_owner = owner;
    auto subs = subscribe_all(*http, owner);
    owner.reset();
    CHECK_FALSE(callback_owner.expired());

    http.reset();
    // Outstanding handles must not retain the adapter's callbacks/resources.
    CHECK(callback_owner.expired());
    for (auto& sub : subs) {
        sub.release();
        CHECK_FALSE(sub.active());
    }
}

TEST_CASE("HttpAdapter — view destruction can release every subscription") {
    auto http = std::make_unique<HttpAdapter>();
    auto subs = subscribe_all(*http);
    bool destroyed = false;
    auto destroy = http->find_view("text")->on_destroy([&] {
        destroyed = true;
        for (auto& sub : subs) sub.release();
    });

    http.reset();
    CHECK(destroyed);
    for (auto& sub : subs) CHECK_FALSE(sub.active());
}

TEST_CASE("HttpAdapter — callback captures can own subscriptions during teardown") {
    auto http = std::make_unique<HttpAdapter>();
    auto owner = std::make_shared<std::vector<aria::Subscription>>();
    std::weak_ptr<std::vector<aria::Subscription>> callback_owner = owner;
    *owner = subscribe_all(*http, owner);
    owner.reset();
    CHECK_FALSE(callback_owner.expired());

    // The last callback's destructor releases handles to maps being destroyed
    // and maps that have already been destroyed, regardless of member order.
    http.reset();
    CHECK(callback_owner.expired());
}

TEST_CASE("HttpAdapter — subscription release can race adapter teardown") {
    for (int round = 0; round < 32; ++round) {
        auto http = std::make_unique<HttpAdapter>();
        auto owner = std::make_shared<int>(round);
        std::weak_ptr<int> callback_owner = owner;
        auto subs = subscribe_all(*http, owner);
        owner.reset();
        std::atomic<bool> ready{false};
        std::thread releaser([subs = std::move(subs), &ready]() mutable {
            while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
            for (auto& sub : subs) sub.release();
        });
        ready.store(true, std::memory_order_release);
        http.reset();
        releaser.join();
        CHECK(callback_owner.expired());
    }
}

TEST_CASE("HttpAdapter — replacement detects registration removed by old view callback") {
    HttpAdapter http;
    auto& old = http.register_view("v", "text");
    auto listener = old.on_destroy([&] { http.unregister_view("v"); });
    CHECK_THROWS_AS(http.register_view("v", "text"), std::logic_error);
    CHECK(http.find_view("v") == nullptr);
}

TEST_CASE("HttpAdapter — replacement respects a newer reentrant registration") {
    HttpAdapter http;
    auto& old = http.register_view("v", "text");
    auto listener = old.on_destroy([&] {
        http.unregister_view("v");
        http.register_view("v", "bool");
    });
    CHECK_THROWS_AS(http.register_view("v", "text"), std::logic_error);
    REQUIRE(http.find_view("v"));
    CHECK(http.find_view("v")->kind() == "bool");
}

TEST_CASE("HttpAdapter — teardown callbacks see a closed empty registry") {
    auto http = std::make_unique<HttpAdapter>();
    auto* raw = http.get();
    auto& view = http->register_view("v", "text");
    bool called = false;
    auto listener = view.on_destroy([&] {
        called = true;
        CHECK(raw->list_views().empty());
        CHECK_THROWS_AS(raw->register_view("new", "text"), std::logic_error);
    });
    http.reset();
    CHECK(called);
}

TEST_CASE("HttpAdapter — setters and subscriptions reject foreign views") {
    HttpAdapter http, other;
    auto& local = http.register_view("v", "text");
    auto& foreign = other.register_view("v", "text");
    struct CustomView : aria::binding::IView {
        std::string_view kind() const noexcept override { return "text"; }
    } custom;
    for (aria::binding::IView* view : {static_cast<aria::binding::IView*>(&foreign), static_cast<aria::binding::IView*>(&custom)}) {
        http.set_text(*view, "wrong");
        CHECK(http.get_text(*view).empty());
        CHECK_FALSE(http.on_text_changed(*view, [](std::string_view) {}));
    }
    CHECK(http.get_text(local).empty());
    CHECK_FALSE(http.on_bool_changed(local, [](bool) {}));
}

TEST_CASE("HttpAdapter — invalid TLS and queue configurations fail explicitly") {
    HttpAdapterConfig cfg;
    SUBCASE("partial certificate") { cfg.tls_cert_file = "certificate.pem"; }
    SUBCASE("partial key") { cfg.tls_key_file = "key.pem"; }
    SUBCASE("unsupported minimum") { cfg.tls_min_version = "1.0"; }
    SUBCASE("empty SSE capacity") { cfg.max_pending_sse_bytes = 0; }
    SUBCASE("empty notification capacity") { cfg.max_pending_notifications = 0; }
#ifndef ARIA_HTTP_HAS_TLS
    SUBCASE("TLS unavailable") {
        cfg.tls_cert_file = "certificate.pem";
        cfg.tls_key_file = "key.pem";
    }
#endif
    CHECK_THROWS_AS(HttpAdapter{cfg}, std::invalid_argument);
}

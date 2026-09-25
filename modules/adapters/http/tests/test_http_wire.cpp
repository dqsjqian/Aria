#include <doctest/doctest.h>
#include "aria/adapters/http/http_adapter.hpp"
#include "aria/binding/binding_engine.hpp"
#include "aria/runtime/dispatcher.hpp"
#include "test_http_client.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <mutex>
#include <queue>
#include <thread>

using namespace aria::adapters::http;
using json = nlohmann::json;
using namespace std::chrono_literals;

namespace {
HttpAdapterConfig config() {
    HttpAdapterConfig c;
    c.port = 0;
    c.worker_threads = 4;
    c.heartbeat_sec = 60;
    return c;
}
class PumpDispatcher final : public aria::runtime::IDispatcher {
    std::thread::id owner = std::this_thread::get_id();
    std::mutex mu;
    std::queue<std::function<void()>> queue;
public:
    bool is_main_thread() const noexcept override { return owner == std::this_thread::get_id(); }
    void post(std::function<void()> fn) override { std::lock_guard lk(mu); queue.push(std::move(fn)); }
    void post_delayed(std::chrono::milliseconds, std::function<void()> fn) override { post(std::move(fn)); }
    void pump() {
        for (;;) {
            std::function<void()> fn;
            { std::lock_guard lk(mu); if (queue.empty()) break; fn = std::move(queue.front()); queue.pop(); }
            fn();
        }
    }
};
}

TEST_CASE("HTTP requests validate schema, view kind and exact numeric range before mutation") {
    HttpAdapter http(config());
    for (const auto* kind : {"text", "bool", "int", "int64", "uint64", "float", "double", "click"})
        http.register_view(kind, kind);
    REQUIRE(http.start());
    test_http::Client c(http.actual_port());
    REQUIRE(c.connected());
    auto check_post = [&](const char* path, const std::string& body, int status) {
        auto r = c.post(path, body);
        CHECK(r.status == status);
        auto payload = json::parse(r.body);
        if (status != 200) CHECK(payload.contains("error"));
    };
    for (const auto* body : {"{", "[]", R"({"view":123,"field":"text","value":"x"})",
            R"({"view":"text","field":true,"value":"x"})",
            R"({"view":"text","field":"bool","value":true})",
            R"({"view":"bool","field":"bool","value":1})",
            R"({"view":"int","field":"int","value":true})",
            R"({"view":"int","field":"int","value":1.5})",
            R"({"view":"int","field":"int","value":2147483648})",
            R"({"view":"uint64","field":"uint64","value":-1})",
            R"({"view":"uint64","field":"uint64","value":"18446744073709551616"})",
            R"({"view":"int64","field":"int64","value":"9223372036854775808"})",
            R"({"view":"int64","field":"int64","value":18446744073709551615})",
            R"({"view":"float","field":"float","value":1e100})",
            R"({"view":"double","field":"double","value":1e400})"})
        check_post("/aria/state", body, 400);
    check_post("/aria/state", R"({"view":"missing","field":"text","value":"x"})", 404);
    check_post("/aria/click", R"({"view":false})", 400);
    check_post("/aria/click", R"({"view":"click","extra":1e400})", 400);
    check_post("/aria/command", R"({"view":"text","command":"c","args":1e400})", 400);
    check_post("/aria/click", R"({"view":"missing"})", 404);
    check_post("/aria/click", R"({"view":"text"})", 400);
    check_post("/aria/click", R"({"view":"click"})", 200);
    check_post("/aria/command", R"({"view":1,"command":2})", 400);
    check_post("/aria/command", R"({"view":"text","command":"missing"})", 404);
    CHECK(http.get_int(*http.find_view("int")) == 0);
    check_post("/aria/state", R"({"view":"int64","field":"int64","value":9007199254740993})", 200);
    CHECK(http.get_int64(*http.find_view("int64")) == 9007199254740993LL);
    auto r = c.get("/aria/state?view=int64");
    CHECK(json::parse(r.body)["value"] == "9007199254740993");
    check_post("/aria/state", R"({"view":"int64","field":"int64","value":"-9223372036854775808"})", 200);
    CHECK(http.get_int64(*http.find_view("int64")) == std::numeric_limits<std::int64_t>::min());
    check_post("/aria/state", R"({"view":"uint64","field":"uint64","value":"18446744073709551615"})", 200);
    CHECK(http.get_uint64(*http.find_view("uint64")) == std::numeric_limits<std::uint64_t>::max());
    check_post("/aria/state", R"({"view":"int64","field":"int64","value":42})", 200);
    r = c.get("/aria/state?view=int64");
    CHECK(json::parse(r.body)["value"] == 42);
}

TEST_CASE("HTTP replacement retires bindings, callbacks, commands and shadow state without deadlock") {
    auto http = std::make_shared<HttpAdapter>(config());
    auto dispatcher = std::make_shared<PumpDispatcher>();
    aria::binding::BindingEngine engine(http, dispatcher,
        aria::binding::BindingEngine::DispatchPolicy::SmartMarshal);
    aria::Property<std::string> old_value("old"), new_value("new");
    auto& old = http->register_view("v", "text");
    engine.bind_text(old_value, old);
    http->set_visible(old, false);
    http->register_command("v", "stale", [](std::string_view) { return "{}"; });
    REQUIRE(http->start());
    test_http::Client c(http->actual_port());
    REQUIRE(c.connected());
    auto r = c.post("/aria/state", R"({"view":"v","field":"text","value":"queued"})");
    CHECK(r.status == 200);
    CHECK(old_value.get() == "old");
    auto& replacement = http->register_view("v", "text");
    CHECK(http->get_text(replacement).empty());
    engine.bind_text(new_value, replacement);
    dispatcher->pump();
    CHECK(old_value.get() == "old");
    CHECK(new_value.get() == "new");
    r = c.post("/aria/command", R"({"view":"v","command":"stale"})");
    CHECK(r.status == 404);
    r = c.post("/aria/state", R"({"view":"v","field":"text","value":"updated"})");
    CHECK(r.status == 200);
    CHECK(new_value.get() == "new");
    dispatcher->pump();
    CHECK(new_value.get() == "updated");
    http->unregister_view("v");
    CHECK(http->find_view("v") == nullptr);
}

TEST_CASE("SSE initial snapshot includes numeric precision, visibility and command enablement") {
    HttpAdapter http(config());
    auto& big = http.register_view("big", "int64");
    auto& button = http.register_view("button", "click");
    http.set_int64(big, 9007199254740993LL);
    http.set_visible(big, false);
    http.set_enabled(button, false);
    REQUIRE(http.start());
    test_http::Stream stream(http.actual_port());
    REQUIRE(stream.wait_for("9007199254740993"));
    // Each initial view ends in enabled; wait until the complete snapshot arrives.
    REQUIRE(stream.wait_for("\"view\":\"button\""));
    http.set_int64(big, 9007199254740995LL);
    REQUIRE(stream.wait_for("9007199254740995"));
    auto events = stream.events();
    CHECK(events.front()["protocol"] == 2);
    bool hidden = false, disabled = false, default_visible = false;
    std::vector<json> values;
    for (auto& e : events) {
        if (e.value("view", "") == "big" && e["type"] == "state") values.push_back(e["value"]);
        if (e.value("view", "") == "big" && e["type"] == "visibility") hidden = e["value"] == false;
        if (e.value("view", "") == "button" && e["type"] == "enabled") disabled = e["value"] == false;
        if (e.value("view", "") == "button" && e["type"] == "visibility") default_visible = e["value"] == true;
    }
    CHECK(hidden);
    CHECK(disabled);
    CHECK(default_visible);
    REQUIRE(values.size() == 2);
    CHECK(values.front() == "9007199254740993");
    CHECK(values.back() == "9007199254740995");
    http.stop();
}

TEST_CASE("SSE capacity responses and REST liveness under connected streams") {
    // With coroutine-per-connection an SSE client no longer occupies a
    // worker, so the cap is purely the configured maximum.
    auto cfg = config();
    cfg.max_sse_clients = 2;
    HttpAdapter http(cfg);
    REQUIRE(http.start());
    test_http::Stream first(http.actual_port()), second(http.actual_port());
    REQUIRE(first.connected());
    REQUIRE(second.connected());
    REQUIRE(first.wait_for("hello"));
    REQUIRE(second.wait_for("hello"));
    CHECK(http.client_count() == 2);
    test_http::Client c(http.actual_port());
    REQUIRE(c.connected());
    auto r = c.get("/aria/stream");
    CHECK(r.status == 503);
    r = c.get("/aria/health");
    CHECK(r.status == 200);
    http.stop();
}

TEST_CASE("HTTP stop interrupts heartbeat, resets port, and permits immediate restart") {
    HttpAdapter http(config());
    for (int i = 0; i < 2; ++i) {
        REQUIRE(http.start());
        test_http::Client c(http.actual_port());
        REQUIRE(c.connected());
        CHECK(c.get("/aria/health").status == 200);
        auto start = std::chrono::steady_clock::now();
        http.stop();
        CHECK(std::chrono::steady_clock::now() - start < 1s);
        CHECK_FALSE(http.running());
        CHECK(http.actual_port() == 0);
    }
    auto invalid = config();
    invalid.worker_threads = 1;
    CHECK_THROWS_AS(HttpAdapter{invalid}, std::invalid_argument);
}

TEST_CASE("HTTP bind failure cleans up and can retry after occupied port is released") {
    // Continuo binds exclusively by default on every platform, so a second
    // adapter on the same port deterministically fails.
    HttpAdapter occupying(config());
    REQUIRE(occupying.start());
    auto cfg = config();
    cfg.port = occupying.actual_port();
    HttpAdapter http(cfg);
    CHECK_FALSE(http.start());
    CHECK_FALSE(http.running());
    CHECK(http.actual_port() == 0);
    occupying.stop();
    REQUIRE(http.start());
    http.stop();
}

TEST_CASE("HTTP keep-alive serves multiple requests on one connection") {
    HttpAdapter http(config());
    http.register_view("v", "int");
    REQUIRE(http.start());
    test_http::Client c(http.actual_port());
    REQUIRE(c.connected());
    // One socket, three exchanges: the connection loop must stay in sync.
    for (int i = 0; i < 3; ++i) {
        auto r = c.post("/aria/state", R"({"view":"v","field":"int","value":7})");
        CHECK(r.status == 200);
        auto health = c.get("/aria/health");
        CHECK(health.status == 200);
    }
    CHECK(http.get_int(*http.find_view("v")) == 7);
    http.stop();
}

TEST_CASE("HEAD and CORS preflight behave on the wire") {
    auto cfg = config();
    cfg.enable_cors = true;
    HttpAdapter http(cfg);
    REQUIRE(http.start());
    test_http::Client c(http.actual_port());
    REQUIRE(c.connected());
    auto options = c.request("OPTIONS", "/aria/state");
    CHECK(options.status == 204);
    CHECK(options.body.empty());
    auto head = c.request("HEAD", "/aria/health");
    CHECK(head.status == 200);
    CHECK(head.body.empty());
    auto get = c.get("/aria/health");
    CHECK(get.status == 200);
    CHECK(json::parse(get.body)["ok"] == true);
    http.stop();
}

TEST_CASE("Static mount serves files, defaults to index.html and refuses traversal") {
    const auto root = std::filesystem::temp_directory_path() /
                      ("aria-http-static-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "assets");
    {
        std::ofstream(root / "index.html") << "<html>home</html>";
        std::ofstream(root / "assets" / "app.js") << "console.log(1)";
    }
    auto cfg = config();
    cfg.static_root = root.string();
    HttpAdapter http(cfg);
    REQUIRE(http.start());
    test_http::Client c(http.actual_port());
    REQUIRE(c.connected());
    auto home = c.get("/index.html");
    CHECK(home.status == 200);
    CHECK(home.body == "<html>home</html>");
    auto script = c.get("/assets/app.js");
    CHECK(script.status == 200);
    CHECK(script.body == "console.log(1)");
    auto index_default = c.get("/");
    CHECK(index_default.status == 200);
    CHECK(index_default.body == "<html>home</html>");
    auto missing = c.get("/nope.html");
    CHECK(missing.status == 404);
    // API routes still win over the static mount.
    auto health = c.get("/aria/health");
    CHECK(health.status == 200);
    http.stop();
    std::filesystem::remove_all(root);
}

TEST_CASE("HTTP callback captures can reenter registry during subscription and command teardown") {
    HttpAdapter http(config());
    auto& view = http.register_view("v", "text");
    int destroyed = 0;
    struct Capture {
        HttpAdapter& http;
        int& destroyed;
        Capture(HttpAdapter& h, int& d) : http(h), destroyed(d) {}
        ~Capture() { static_cast<void>(http.list_views()); ++destroyed; }
    };
    auto capture = std::make_shared<Capture>(http, destroyed);
    auto subscription = http.on_text_changed(view, [capture](std::string_view) {});
    capture.reset();
    subscription.release();
    CHECK(destroyed == 1);
    capture = std::make_shared<Capture>(http, destroyed);
    http.register_command("v", "c", [capture](std::string_view) { return "{}"; });
    capture.reset();
    http.register_command("v", "c", [](std::string_view) { return "{}"; });
    CHECK(destroyed == 2);
    capture = std::make_shared<Capture>(http, destroyed);
    http.register_command("v", "c", [capture](std::string_view) { return "{}"; });
    capture.reset();
    http.unregister_command("v", "c");
    CHECK(destroyed == 3);
}

TEST_CASE("HTTP state notifications follow commit order and bounded admission") {
    auto cfg = config();
    cfg.max_pending_notifications = 1;
    HttpAdapter http(cfg);
    auto& view = http.register_view("v", "int");
    std::promise<void> entered, release;
    auto entered_future = entered.get_future();
    auto release_future = release.get_future().share();
    std::mutex seen_mu;
    std::vector<int> seen;
    auto sub = http.on_int_changed(view, [&](int value) {
        if (value == 1) {
            entered.set_value();
            release_future.wait();
        }
        std::lock_guard lock(seen_mu);
        seen.push_back(value);
    });
    REQUIRE(http.start());
    const int port = http.actual_port();
    std::atomic<int> first_status{0};
    std::thread first([&] {
        test_http::Client client(static_cast<std::uint16_t>(port));
        if (!client.connected()) return;
        auto result = client.post("/aria/state", R"({"view":"v","field":"int","value":1})");
        first_status = result.status;
    });
    const bool started = entered_future.wait_for(2s) == std::future_status::ready;
    if (!started) {
        release.set_value();
        first.join();
        REQUIRE(started);
    }
    test_http::Client client(static_cast<std::uint16_t>(port));
    REQUIRE(client.connected());
    auto second = client.post("/aria/state", R"({"view":"v","field":"int","value":2})");
    auto excess = client.post("/aria/state", R"({"view":"v","field":"int","value":3})");
    const int committed = http.get_int(view);
    release.set_value();
    first.join();
    CHECK(first_status.load() == 200);
    CHECK(second.status == 200);
    CHECK(excess.status == 503);
    CHECK(committed == 2);
    CHECK(seen == std::vector<int>{1, 2});
}

TEST_CASE("HTTP callback identity survives requests and disconnection cancels pending fanout") {
    HttpAdapter http(config());
    auto& view = http.register_view("v", "int");
    std::vector<int> counters;
    aria::Subscription later;
    auto first = http.on_int_changed(view, [&, count = 0](int) mutable {
        counters.push_back(++count);
        later.release();
    });
    int later_calls = 0;
    later = http.on_int_changed(view, [&](int) { ++later_calls; });
    auto throwing = http.on_int_changed(view, [](int) { throw std::runtime_error("expected callback failure"); });
    int final_calls = 0;
    auto last = http.on_int_changed(view, [&](int) { ++final_calls; });
    REQUIRE(http.start());
    test_http::Client client(http.actual_port());
    REQUIRE(client.connected());
    for (int i = 1; i <= 2; ++i) {
        auto result = client.post("/aria/state",
            json{{"view", "v"}, {"field", "int"}, {"value", i}}.dump());
        CHECK(result.status == 200);
    }
    http.stop();
    CHECK(counters == std::vector<int>{1, 2});
    CHECK(later_calls == 0);
    CHECK(final_calls == 2);
}

TEST_CASE("HTTP lifecycle calls from server tasks fail promptly") {
    // The old backend injected a native route for this; commands now cover
    // the same guarantee — start/stop reject calls from inside server tasks.
    HttpAdapter http(config());
    http.register_view("v", "text");
    http.register_command("v", "lifecycle", [&](std::string_view) {
        int rejected = 0;
        try { http.stop(); } catch (const std::logic_error&) { ++rejected; }
        try { http.start(); } catch (const std::logic_error&) { ++rejected; }
        return std::to_string(rejected);
    });
    REQUIRE(http.start());
    test_http::Client client(http.actual_port());
    REQUIRE(client.connected());
    auto result = client.post("/aria/command", R"({"view":"v","command":"lifecycle"})");
    CHECK(result.status == 200);
    CHECK(result.body == "2");
    http.stop();
}

TEST_CASE("HTTP callback can destroy its adapter while native work finishes") {
    auto http = std::make_unique<HttpAdapter>(config());
    auto& button = http->register_view("button", "click");
    std::promise<void> destroyed;
    auto destroyed_future = destroyed.get_future();
    auto sub = http->on_click(button, [&] {
        http.reset();
        destroyed.set_value();
    });
    REQUIRE(http->start());
    const int port = http->actual_port();
    test_http::Client client(static_cast<std::uint16_t>(port));
    REQUIRE(client.connected());
    // Shutdown can close the socket before a response; completion is observed
    // through the callback, independently of that transport race. Fire the
    // request over the raw socket and drain whatever comes back without any
    // response expectations.
    {
        const std::string request =
            "POST /aria/click HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "Content-Type: application/json\r\nContent-Length: 17\r\n"
            "Connection: close\r\n\r\n" + std::string(R"({"view":"button"})");
        std::string_view sent(request);
        while (!sent.empty()) {
#if defined(_WIN32)
            const int n = ::send(client.raw(), sent.data(), static_cast<int>(sent.size()), 0);
#else
            const ssize_t n = ::send(client.raw(), sent.data(), sent.size(), 0);
#endif
            if (n <= 0) break;  // hung up before the request fully landed: fine
            sent.remove_prefix(static_cast<std::size_t>(n));
        }
        char drain[512];
#if defined(_WIN32)
        while (::recv(client.raw(), drain, sizeof(drain), 0) > 0) {}
#else
        while (::recv(client.raw(), drain, sizeof(drain), 0) > 0) {}
#endif
    }
    REQUIRE(destroyed_future.wait_for(2s) == std::future_status::ready);
    CHECK_FALSE(http);
}

TEST_CASE("HTTP SSE rejects an initial snapshot larger than the configured buffer") {
    auto cfg = config();
    cfg.max_pending_sse_bytes = 128;
    HttpAdapter http(cfg);
    auto& view = http.register_view("v", "text");
    http.set_text(view, std::string(1024, 'x'));
    REQUIRE(http.start());
    test_http::Client client(http.actual_port());
    REQUIRE(client.connected());
    auto result = client.get("/aria/stream");
    CHECK(result.status == 503);
    CHECK(http.client_count() == 0);
}

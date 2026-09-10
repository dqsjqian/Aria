#include <doctest/doctest.h>
#include "aria/adapters/http/http_adapter.hpp"
#include "aria/binding/binding_engine.hpp"
#include "aria/runtime/dispatcher.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <condition_variable>
#include <functional>
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
void timeouts(httplib::Client& c) {
    c.set_connection_timeout(2, 0);
    c.set_read_timeout(2, 0);
    c.set_write_timeout(2, 0);
}
struct Stream {
    httplib::Client client;
    std::mutex mu;
    std::condition_variable cv;
    std::string frames;
    std::thread thread;
    explicit Stream(int port) : client("127.0.0.1", port) {
        timeouts(client);
        thread = std::thread([this] {
            client.Get("/aria/stream", [this](const char* data, std::size_t size) {
                { std::lock_guard lk(mu); frames.append(data, size); }
                cv.notify_all();
                return true;
            });
        });
    }
    ~Stream() { client.stop(); if (thread.joinable()) thread.join(); }
    bool wait_for(std::string_view text) {
        std::unique_lock lk(mu);
        return cv.wait_for(lk, 2s, [&] { return frames.find(text) != std::string::npos; });
    }
    std::vector<json> events() {
        std::lock_guard lk(mu);
        std::vector<json> out;
        std::size_t start = 0;
        while ((start = frames.find("data: ", start)) != std::string::npos) {
            auto end = frames.find("\n\n", start);
            if (end == std::string::npos) break;
            out.push_back(json::parse(frames.substr(start + 6, end - start - 6)));
            start = end + 2;
        }
        return out;
    }
};
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
    httplib::Client c("127.0.0.1", http.actual_port());
    timeouts(c);
    auto check_post = [&](const char* path, const std::string& body, int status) {
        auto r = c.Post(path, body, "application/json");
        REQUIRE(r);
        CHECK(r->status == status);
        auto payload = json::parse(r->body);
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
    auto r = c.Get("/aria/state?view=int64");
    REQUIRE(r);
    CHECK(json::parse(r->body)["value"] == "9007199254740993");
    check_post("/aria/state", R"({"view":"int64","field":"int64","value":"-9223372036854775808"})", 200);
    CHECK(http.get_int64(*http.find_view("int64")) == std::numeric_limits<std::int64_t>::min());
    check_post("/aria/state", R"({"view":"uint64","field":"uint64","value":"18446744073709551615"})", 200);
    CHECK(http.get_uint64(*http.find_view("uint64")) == std::numeric_limits<std::uint64_t>::max());
    check_post("/aria/state", R"({"view":"int64","field":"int64","value":42})", 200);
    r = c.Get("/aria/state?view=int64");
    REQUIRE(r);
    CHECK(json::parse(r->body)["value"] == 42);
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
    httplib::Client c("127.0.0.1", http->actual_port());
    timeouts(c);
    auto r = c.Post("/aria/state", R"({"view":"v","field":"text","value":"queued"})", "application/json");
    REQUIRE(r);
    CHECK(old_value.get() == "old");
    auto& replacement = http->register_view("v", "text");
    CHECK(http->get_text(replacement).empty());
    engine.bind_text(new_value, replacement);
    dispatcher->pump();
    CHECK(old_value.get() == "old");
    CHECK(new_value.get() == "new");
    r = c.Post("/aria/command", R"({"view":"v","command":"stale"})", "application/json");
    REQUIRE(r);
    CHECK(r->status == 404);
    r = c.Post("/aria/state", R"({"view":"v","field":"text","value":"updated"})", "application/json");
    REQUIRE(r);
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
    Stream stream(http.actual_port());
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

TEST_CASE("SSE admission reserves a fixed-pool worker for REST and capacity responses") {
    auto cfg = config();
    cfg.worker_threads = 3;
    cfg.max_sse_clients = 0;
    HttpAdapter http(cfg);
    REQUIRE(http.start());
    Stream first(http.actual_port()), second(http.actual_port());
    REQUIRE(first.wait_for("hello"));
    REQUIRE(second.wait_for("hello"));
    CHECK(http.client_count() == 2);
    httplib::Client c("127.0.0.1", http.actual_port());
    timeouts(c);
    auto r = c.Get("/aria/stream");
    REQUIRE(r);
    CHECK(r->status == 503);
    r = c.Get("/aria/health");
    REQUIRE(r);
    CHECK(r->status == 200);
    http.stop();
}

TEST_CASE("HTTP stop interrupts heartbeat, resets port, and permits immediate restart") {
    HttpAdapter http(config());
    for (int i = 0; i < 2; ++i) {
        REQUIRE(http.start());
        httplib::Client c("127.0.0.1", http.actual_port());
        timeouts(c);
        REQUIRE(c.Get("/aria/health"));
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
    HttpAdapter occupying(config());
    // cpp-httplib defaults to SO_REUSEPORT on Unix. Make the fixture
    // exclusive so the second bind deterministically fails on every host.
    occupying.native_server().set_socket_options([](socket_t socket) {
#ifdef _WIN32
        httplib::set_socket_opt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
        static_cast<void>(socket); // Fresh socket: no SO_REUSEPORT/ADDR.
#endif
    });
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

TEST_CASE("HTTP start waits for task queue readiness and cleans up listener exceptions") {
    HttpAdapter http(config());
    auto original_factory = http.native_server().new_task_queue;
    std::mutex mu;
    std::condition_variable cv;
    bool entered = false, release = false;
    std::atomic<bool> start_finished{false};
    bool start_result = true;
    http.native_server().new_task_queue = [&]() -> httplib::TaskQueue* {
        std::unique_lock lk(mu);
        entered = true;
        cv.notify_all();
        cv.wait(lk, [&] { return release; });
        throw std::runtime_error("injected task queue failure");
    };
    std::thread starter([&] { start_result = http.start(); start_finished = true; });
    {
        std::unique_lock lk(mu);
        const bool reached = cv.wait_for(lk, 2s, [&] { return entered; });
        CHECK(reached);
        CHECK_FALSE(start_finished.load());
        release = true;
    }
    cv.notify_all();
    starter.join();
    CHECK_FALSE(start_result);
    CHECK_FALSE(http.running());
    CHECK(http.actual_port() == 0);
    http.native_server().new_task_queue = original_factory;
    REQUIRE(http.start());
    httplib::Client client("127.0.0.1", http.actual_port());
    timeouts(client);
    REQUIRE(client.Get("/aria/health"));
    http.stop();
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

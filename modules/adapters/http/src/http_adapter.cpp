/// @file http_adapter.cpp
/// @brief HTTP/REST/SSE implementation built on Mira + nlohmann::json.
///
/// Mira (MIT, https://github.com/dqsjqian/Mira) supplies the event
/// loop, TCP, optional TLS, and the HTTP/1.1 connection loop. nlohmann::json
/// (MIT) handles JSON encode/decode. Both are hash-pinned downloads — see
/// cmake/ariaFetchPinned.cmake and the module CMakeLists.
///
/// Threading model:
///   - **Loop thread** — runs `EventLoop::run_until_complete` over a root
///     task that owns the accept loop, every connection coroutine, and the
///     heartbeat timer. All socket I/O, TLS, and SSE streaming live here.
///     Because SSE coroutines park on the loop instead of blocking a worker,
///     a slow SSE client costs a connection buffer, not a thread.
///   - **Worker pool** (`worker_threads`, >= 2) — synchronous route logic:
///     JSON parsing, registry access, user command handlers, and the
///     notification drain that wakes BindingEngine callbacks. This keeps
///     the "handlers run on workers" contract of the previous backend
///     while the loop stays responsive.
///   - **Caller thread** — `start()` / `stop()` lifecycle.
///
/// We own:
///   - the wire protocol (see wire_protocol.hpp)
///   - the view registry + shadow state
///   - subscription dispatch (text/bool/numeric/click)
///   - SSE fan-out across connected clients

#include "aria/adapters/http/http_adapter.hpp"
#include "aria/adapters/http/wire_protocol.hpp"
#include "aria/binding/view_adapter.hpp"
#include "aria/callback_boundary.hpp"

#include <mira/core/event_loop.hpp>
#include <mira/core/executor.hpp>
#include <mira/core/task.hpp>
#include <mira/core/task_scope.hpp>
#include <mira/http/connection.hpp>
#include <mira/transport/tcp.hpp>
#if defined(ARIA_HTTP_HAS_TLS)
#  include <mira/tls/context.hpp>
#  include <mira/tls/stream.hpp>
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aria::adapters::http {

using json = nlohmann::json;

namespace {

using Mira::EventLoop;
using Mira::OperationOptions;
using Mira::Result;
using Mira::Task;

std::span<const std::byte> as_bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

// ─────────────────────────────────────────────────────────────────────────────
// Worker pool — where synchronous route logic and user callbacks run.
//
// Mira deliberately owns no thread policy, so the adapter brings its own
// tiny pool. Jobs are opaque callables; the pool never touches sockets.
// ─────────────────────────────────────────────────────────────────────────────

/// Shared queue + stop flag for the worker threads. Workers are created
/// once per adapter and joined by the loop thread as it exits, mirroring
/// how the previous backend joined its pool before the listener returned.
/// Because a callback may destroy the adapter while a worker runs it, the
/// state outlives any single teardown path.
struct PoolState {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::function<void()>> jobs;
    bool stopped = false;

    void post(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(mu);
            if (stopped) return;
            jobs.push_back(std::move(job));
        }
        cv.notify_one();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mu);
            stopped = true;
        }
        cv.notify_all();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// SSE per-client outbox.
//
// Any thread (the binding engine, a Property setter, the heartbeat timer on
// the loop) can enqueue a frame without blocking on the network write. The
// SSE coroutine parks on the loop between frames and is resumed through
// `EventLoop::post`, so a parked stream occupies no worker thread.
// ─────────────────────────────────────────────────────────────────────────────

struct SseClient {
    explicit SseClient(std::size_t limit) : max_pending_bytes(limit) {}

    std::deque<std::string> queue;
    std::size_t queued_bytes = 0;
    const std::size_t max_pending_bytes;
    std::mutex mu;
    std::coroutine_handle<> waiter{};  // parked SSE coroutine, if any
    EventLoop* loop = nullptr;         // where the waiter resumes
    std::atomic<bool> closed{false};
    bool overflowed = false;           // slow-client eviction, guarded by mu

    void push(std::string_view frame) {
        std::coroutine_handle<> resume;
        {
            std::lock_guard<std::mutex> lock(mu);
            if (closed) return;
            if (frame.size() > max_pending_bytes - queued_bytes) {
                closed = true;
                overflowed = true;
                queue.clear();
                queued_bytes = 0;
                resume = std::exchange(waiter, {});
            } else {
                queue.emplace_back(frame);
                queued_bytes += frame.size();
                if (queue.size() == 1) resume = std::exchange(waiter, {});
            }
        }
        if (resume && loop) {
            loop->post([resume] { resume.resume(); });
        }
    }

    void close() {
        std::coroutine_handle<> resume;
        {
            std::lock_guard<std::mutex> lock(mu);
            if (closed) return;
            closed = true;
            queue.clear();
            queued_bytes = 0;
            resume = std::exchange(waiter, {});
        }
        if (resume && loop) {
            loop->post([resume] { resume.resume(); });
        }
    }

    /// True when a frame is ready or the client is gone. Callers hold mu.
    bool ready_locked() const noexcept {
        return closed || !queue.empty();
    }
};

/// Suspend the SSE coroutine until its outbox has something to send.
struct SseWait {
    SseClient* client;

    bool await_ready() const noexcept {
        std::lock_guard<std::mutex> lock(client->mu);
        return client->ready_locked();
    }

    void await_suspend(std::coroutine_handle<> waiting) {
        // Under the same lock that push/close use to hand the handle out,
        // so a frame arriving between the check and the park still wakes us.
        std::lock_guard<std::mutex> lock(client->mu);
        if (client->ready_locked()) {
            waiting.resume();  // already ready: continue inline
            return;
        }
        client->waiter = waiting;
    }

    void await_resume() const noexcept {}
};

inline std::string sse_frame(const std::string& dump) {
    std::string out;
    out.reserve(dump.size() + 8);
    out += "data: ";
    out += dump;
    out += "\n\n";
    return out;
}

// Keep native shadow values exact; only the JSON wire representation changes.
json wire_value(std::string_view kind, const json& value) {
    constexpr std::int64_t safe = 9007199254740991LL;
    if (kind == wire::field_kinds::kInt64 && value.is_number_integer()) {
        auto n = value.get<std::int64_t>();
        if (n < -safe || n > safe) return std::to_string(n);
    }
    if (kind == wire::field_kinds::kUInt64 && value.is_number_integer()) {
        auto n = value.get<std::uint64_t>();
        if (n > static_cast<std::uint64_t>(safe)) return std::to_string(n);
    }
    return value;
}

template<class T>
T exact_integer(const json& value, bool allow_string = false) {
    if (allow_string && value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        T n{};
        auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), n);
        if (ec == std::errc{} && end == text.data() + text.size()) return n;
    } else if (value.is_number_unsigned()) {
        auto n = value.get<std::uint64_t>();
        if (n <= static_cast<std::uint64_t>(std::numeric_limits<T>::max()))
            return static_cast<T>(n);
    } else if (value.is_number_integer()) {
        auto n = value.get<std::int64_t>();
        if constexpr (std::is_unsigned_v<T>) {
            if (n >= 0 && static_cast<std::uint64_t>(n) <= std::numeric_limits<T>::max())
                return static_cast<T>(n);
        } else {
            if (n >= std::numeric_limits<T>::min() && n <= std::numeric_limits<T>::max())
                return static_cast<T>(n);
        }
    }
    throw std::invalid_argument("expected an exact integer in range");
}

template<class T>
T finite_number(const json& value) {
    if (!value.is_number()) throw std::invalid_argument("expected a number");
    double n = value.get<double>();
    if (!std::isfinite(n) || n < -static_cast<double>(std::numeric_limits<T>::max()) ||
        n > static_cast<double>(std::numeric_limits<T>::max()))
        throw std::invalid_argument("number out of range");
    return static_cast<T>(n);
}

// ── URL helpers ──────────────────────────────────────────────────────────────

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// Percent-decode a path/query component; '+' decodes to space (form style).
std::string url_decode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int high = hex_value(text[i + 1]);
            const int low = hex_value(text[i + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i] == '+' ? ' ' : text[i]);
    }
    return out;
}

/// First value of `key` in a query string, or empty when absent.
std::string query_param(std::string_view query, std::string_view key) {
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = query.find('&', start);
        const std::string_view pair =
            query.substr(start, end == std::string_view::npos ? std::string_view::npos
                                                              : end - start);
        const std::size_t eq = pair.find('=');
        const std::string_view name =
            eq == std::string_view::npos ? pair : pair.substr(0, eq);
        if (name == key) {
            return url_decode(eq == std::string_view::npos ? std::string_view{}
                                                           : pair.substr(eq + 1));
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return {};
}

/// Content-Type for a static asset, by extension.
std::string_view static_content_type(std::string_view path) {
    const std::size_t dot = path.rfind('.');
    if (dot == std::string_view::npos) return "application/octet-stream";
    const std::string_view ext = path.substr(dot + 1);
    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "js" || ext == "mjs") return "text/javascript; charset=utf-8";
    if (ext == "css") return "text/css; charset=utf-8";
    if (ext == "json" || ext == "map") return "application/json";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "txt") return "text/plain; charset=utf-8";
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "webp") return "image/webp";
    if (ext == "ico") return "image/x-icon";
    if (ext == "woff") return "font/woff";
    if (ext == "woff2") return "font/woff2";
    if (ext == "xml") return "application/xml";
    return "application/octet-stream";
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// HttpAdapter::Impl
// ─────────────────────────────────────────────────────────────────────────────

struct HttpAdapter::Impl : std::enable_shared_from_this<HttpAdapter::Impl> {
    inline static thread_local Impl* executing = nullptr;
    struct ExecutionScope {
        Impl* previous;
        explicit ExecutionScope(Impl* owner) : previous(std::exchange(executing, owner)) {}
        ~ExecutionScope() { executing = previous; }
    };

    HttpAdapterConfig config;
    std::size_t pool_size = 0;
    std::shared_ptr<PoolState> pool;
    std::vector<std::thread> workers;
    std::optional<EventLoop> loop;
    std::thread loop_thread;
    std::atomic<bool> running{false};
    std::atomic<std::uint16_t> bound_port{0};
    bool tls_active = false;
    std::string tls_error;  // non-empty when TLS configuration failed at start
#if defined(ARIA_HTTP_HAS_TLS)
    std::optional<Mira::tls::Context> tls_context;
#endif
    std::optional<Mira::transport::tcp::Listener> listener;
    std::stop_source stop_source;
    std::mutex lifecycle_mu;

    // A disconnector briefly locks weak Impl ownership before accessing the
    // registry; outstanding subscriptions do not keep callbacks alive.
    std::mutex registry_mu;
    bool registry_open = true;
    std::deque<std::function<void()>> notifications;
    bool notifying = false;
    std::unordered_map<std::string, std::shared_ptr<HttpView>> views;
    std::unordered_map<std::string, json> shadow;
    std::unordered_map<std::string, bool> shadow_visible;
    std::unordered_map<std::string, bool> shadow_enabled;

    // Snapshots retain callback identity instead of copying user callables.
    // A disconnect invalidates not-yet-started calls in an active snapshot.
    template<class Fn>
    struct CallbackNode {
        explicit CallbackNode(Fn fn) : callback(std::move(fn)) {}
        std::atomic<bool> active{true};
        Fn callback;
    };
    template<class Fn>
    using Callbacks = std::unordered_map<std::string,
        std::map<std::uint64_t, std::shared_ptr<CallbackNode<Fn>>>>;
    Callbacks<std::function<void(std::string_view)>> on_text;
    Callbacks<std::function<void(bool)>> on_bool;
    Callbacks<std::function<void(int)>> on_int;
    Callbacks<std::function<void(std::int64_t)>> on_int64;
    Callbacks<std::function<void(std::uint64_t)>> on_uint64;
    Callbacks<std::function<void(float)>> on_float;
    Callbacks<std::function<void(double)>> on_double;
    Callbacks<std::function<void()>> on_click;
    std::uint64_t next_sub_id = 0;

    std::map<std::pair<std::string, std::string>,
        std::shared_ptr<CallbackNode<CommandHandler>>> commands;

    mutable std::mutex sse_mu;
    std::vector<std::shared_ptr<SseClient>> sse_clients;

    // The effective SSE admission cap. With coroutine-per-connection the old
    // "one worker per SSE client" bound is gone; `max_sse_clients` is the
    // whole story now (0 = a sane default).
    std::size_t sse_capacity = 0;

    explicit Impl(HttpAdapterConfig c) : config(std::move(c)) {
        pool_size = make_pool_size(config);
        pool = std::make_shared<PoolState>();
        auto created = EventLoop::create();
        if (!created) throw std::runtime_error("HTTP event loop creation failed");
        loop = std::move(*created);
        if (config.heartbeat_sec <= 0 || config.max_sse_clients < 0 ||
            config.max_pending_sse_bytes == 0 || config.max_pending_notifications == 0)
            throw std::invalid_argument("invalid HTTP worker, heartbeat or SSE limit");
        sse_capacity = config.max_sse_clients == 0
            ? 64
            : static_cast<std::size_t>(config.max_sse_clients);
        const bool cert = !config.tls_cert_file.empty();
        const bool key = !config.tls_key_file.empty();
        if (cert != key || (!config.tls_ca_file.empty() && !cert))
            throw std::invalid_argument("TLS requires both certificate and private key");
        if (config.tls_min_version != "1.2" && config.tls_min_version != "1.3")
            throw std::invalid_argument("TLS minimum version must be 1.2 or 1.3");
        if (cert) {
#if defined(ARIA_HTTP_HAS_TLS)
            construct_tls_context();
#else
            throw std::invalid_argument(
                "TLS configured but this HTTP adapter was built without TLS support");
#endif
        }
    }

    static std::size_t make_pool_size(const HttpAdapterConfig& c) {
        if (c.worker_threads < 0 || c.worker_threads == 1)
            throw std::invalid_argument("invalid HTTP worker count");
        return c.worker_threads == 0
            ? std::max(2u, std::thread::hardware_concurrency())
            : static_cast<std::size_t>(c.worker_threads);
    }

    ~Impl() {
        stop_server();
        pool->stop();
        // Workers were joined by the loop thread (or detached with it), so
        // no joinable std::thread is ever destroyed here.
        close_registry();
    }

    // ── TLS construction (maps config onto Mira's server context) ──

#if defined(ARIA_HTTP_HAS_TLS)
    void construct_tls_context() {
        Mira::tls::Context::ServerConfig tls;
        tls.cert_file = config.tls_cert_file;
        tls.key_file = config.tls_key_file;
        if (!config.tls_ca_file.empty()) {
            // Mira enforces peer verification whenever a CA is loaded;
            // there is no insecure bypass, matching the adapter's contract.
            tls.client_ca_file = config.tls_ca_file;
        }
        tls.min_version = config.tls_min_version;
        auto built = Mira::tls::Context::server(tls);
        if (!built) {
            tls_error = built.error().message();
            return;
        }
        tls_context = std::move(*built);
        tls_active = true;
    }
#endif

    template<class Map>
    static void deactivate_callbacks(Map& callbacks) noexcept {
        for (auto& [_, rows] : callbacks)
            for (auto& [id, node] : rows) node->active.store(false, std::memory_order_release);
    }

    void close_registry() noexcept {
        std::unique_lock lock(registry_mu);
        registry_open = false;
        deactivate_callbacks(on_text); deactivate_callbacks(on_bool);
        deactivate_callbacks(on_int); deactivate_callbacks(on_int64);
        deactivate_callbacks(on_uint64); deactivate_callbacks(on_float);
        deactivate_callbacks(on_double); deactivate_callbacks(on_click);
        for (auto& [_, node] : commands) node->active.store(false, std::memory_order_release);
        auto retired = std::tuple{std::move(views), std::move(shadow),
            std::move(shadow_visible), std::move(shadow_enabled),
            std::move(on_text), std::move(on_bool), std::move(on_int),
            std::move(on_int64), std::move(on_uint64), std::move(on_float),
            std::move(on_double), std::move(on_click), std::move(commands),
            std::move(notifications)};
        lock.unlock();
        // The facade still owns Impl while arbitrary captures and views retire.
    }

    HttpView* owned_view_locked(binding::IView& view, std::string_view kind = {}) {
        if (!registry_open) return nullptr;
        auto* http = dynamic_cast<HttpView*>(&view);
        if (!http || (!kind.empty() && http->kind() != kind)) return nullptr;
        auto it = views.find(http->id());
        return it != views.end() && it->second.get() == http ? http : nullptr;
    }

    template<class Map, class Fn>
    Subscription subscribe(binding::IView& view, std::string_view kind, Map& callbacks, Fn callback) {
        if (!callback) return {};
        auto node = std::make_shared<CallbackNode<Fn>>(std::move(callback));
        std::string view_id;
        std::uint64_t id;
        {
            std::lock_guard lock(registry_mu);
            auto* http = owned_view_locked(view, kind);
            if (!http) return {};
            if (next_sub_id == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("HTTP subscription identifiers exhausted");
            id = ++next_sub_id;
            view_id = http->id();
            callbacks[view_id].emplace(id, node);
        }
        auto disconnect = [weak = weak_from_this(), registry = &callbacks, view_id = std::move(view_id), id] {
            auto owner = weak.lock();
            if (!owner) return;
            std::unique_lock lock(owner->registry_mu);
            if (!owner->registry_open) return;
            auto group = registry->find(view_id);
            if (group == registry->end()) return;
            auto found = group->second.find(id);
            if (found == group->second.end()) return;
            found->second->active.store(false, std::memory_order_release);
            auto retired = group->second.extract(found);
            if (group->second.empty()) registry->erase(group);
            lock.unlock();
        };
        try { return Subscription{disconnect}; }
        catch (...) { disconnect(); throw; }
    }

    // Called with registry_mu held. The first committing worker drains;
    // reentrant/concurrent requests append and return after admission.
    bool enqueue_notification(std::function<void()> callback) {
        notifications.push_back(std::move(callback));
        return !std::exchange(notifying, true);
    }

    void drain_notifications() {
        auto owner = shared_from_this();
        for (;;) {
            std::function<void()> callback;
            {
                std::lock_guard lock(registry_mu);
                if (!registry_open || notifications.empty()) {
                    notifying = false;
                    return;
                }
                callback = std::move(notifications.front());
                notifications.pop_front();
            }
            // Callback and capture destruction both occur outside the lock.
            try { callback(); }
            catch (...) { report_callback_failure("http.notification", std::current_exception()); }
        }
    }

    template<class Nodes, class... Args>
    static void notify_callbacks(const Nodes& nodes, Args&&... args) noexcept {
        for (const auto& node : nodes) {
            if (!node->active.load(std::memory_order_acquire)) continue;
            try { node->callback(std::forward<Args>(args)...); }
            catch (...) { report_callback_failure("http.callback", std::current_exception()); }
        }
    }

    // ── SSE fan-out (thread-safe; callable from any thread) ───────────

    void broadcast_state(const std::string& view_id,
                         std::string_view field,
                         const json& value) {
        json env = {
            {wire::fields::kType,  wire::event_types::kState},
            {wire::fields::kView,  view_id},
            {wire::fields::kField, std::string(field)},
            {wire::fields::kValue, wire_value(field, value)},
        };
        broadcast(env.dump());
    }

    void broadcast(const std::string& dump) {
        std::string frame = sse_frame(dump);
        std::vector<std::shared_ptr<SseClient>> snapshot;
        {
            std::lock_guard<std::mutex> lock(sse_mu);
            snapshot = sse_clients;
        }
        for (auto& client : snapshot) {
            if (!client->closed) client->push(frame);
        }
    }

    void broadcast_event(const std::string& view_id, std::string_view field) {
        json env = {
            {wire::fields::kType,  wire::event_types::kEvent},
            {wire::fields::kView,  view_id},
            {wire::fields::kField, std::string(field)},
        };
        broadcast(env.dump());
    }

    json list_views_json() {
        std::lock_guard<std::mutex> lock(registry_mu);
        json arr = json::array();
        for (auto& [id, view] : views) {
            arr.push_back({
                {"id",   id},
                {"kind", std::string(view->kind())},
            });
        }
        return {{"views", arr}};
    }

    void remove_sse_client(const std::shared_ptr<SseClient>& client) {
        std::lock_guard<std::mutex> lock(sse_mu);
        sse_clients.erase(
            std::remove(sse_clients.begin(), sse_clients.end(), client),
            sse_clients.end());
    }

    // ── Route dispatch ─────────────────────────────────────────────────
    //
    // One coroutine per connection serves requests. For everything except
    // SSE, the synchronous route logic hops to the worker pool and the
    // computed response comes back to the loop for the actual socket write.

    // A computed response, produced on the pool, written on the loop.
    struct RouteOutcome {
        unsigned status = 200;
        std::string content_type = "application/json";
        std::string body;
    };

    static RouteOutcome error_outcome(unsigned code, std::string message) {
        RouteOutcome out;
        out.status = code;
        out.body = json{{"error", std::move(message)}}.dump();
        return out;
    }

    struct ParsedTarget {
        std::string_view path;
        std::string_view query;
    };

    static ParsedTarget split_target(std::string_view target) {
        const std::size_t question = target.find('?');
        if (question == std::string_view::npos) return {target, {}};
        return {target.substr(0, question), target.substr(question + 1)};
    }

    enum class Route { health, views, state_get, state_post, click, command, stream, other };

    static Route classify(std::string_view method,
                          std::string_view path,
                          const std::string& prefix) {
        if (path.size() <= prefix.size() || path.substr(0, prefix.size()) != prefix) {
            return Route::other;
        }
        const std::string_view rest = path.substr(prefix.size());
        const auto is = [rest](std::string_view suffix) { return rest == suffix; };
        if (method == "GET" || method == "HEAD") {
            if (is("/health")) return Route::health;
            if (is("/views")) return Route::views;
            if (is("/state")) return Route::state_get;
            if (is("/stream")) return Route::stream;
        }
        if (method == "POST") {
            if (is("/state")) return Route::state_post;
            if (is("/click")) return Route::click;
            if (is("/command")) return Route::command;
        }
        return Route::other;
    }

    // The single HTTP handler handed to `serve_connection`. Starts on the
    // loop thread; synchronous work hops to the pool and back.
    template<class Stream>
    Task<Result<void>> serve_request(
        const Mira::http::Request& request,
        Mira::http::ResponseWriter<Stream>& writer,
        std::span<const std::byte> body);

    // Shared synchronous dispatcher: runs ON THE POOL, returns the response.
    RouteOutcome dispatch(const Mira::http::Request& request,
                          std::span<const std::byte> body);

    RouteOutcome handle_health();
    RouteOutcome handle_views();
    RouteOutcome handle_get_state(std::string_view query);
    RouteOutcome handle_post_state(std::span<const std::byte> body_bytes);
    RouteOutcome handle_post_click(std::span<const std::byte> body_bytes);
    RouteOutcome handle_post_command(std::span<const std::byte> body_bytes);
    RouteOutcome handle_static(const Mira::http::Request& request);

    // SSE: admitted on the loop, streamed on the loop until the client or
    // the server goes away. No worker thread is involved.
    template<class Stream>
    Task<Result<void>> run_sse(Mira::http::ResponseWriter<Stream>& writer);

    // ── Connection & accept loops (loop thread only) ───────────────────

    Task<void> connection_task_plain(Mira::transport::tcp::Socket socket);
#if defined(ARIA_HTTP_HAS_TLS)
    Task<void> connection_task_tls(Mira::transport::tcp::Socket socket);
#endif
    Task<void> accept_loop();

    // Heartbeat: periodic SSE pings, driven by the loop's timer wheel.
    Task<void> heartbeat_loop();

    // ── Lifecycle ──────────────────────────────────────────────────────

    bool start_server();
    void stop_locked();
    void stop_server();
    void shutdown() noexcept;

    // Caller holds registry_mu: mutation and publication form one ordered commit.
    template<class T, class Map>
    std::function<void()> commit_state(const std::string& id, const std::string& field,
                                       T value, Map& callbacks) {
        std::vector<typename Map::mapped_type::mapped_type> snapshot;
        auto it = callbacks.find(id);
        if (it != callbacks.end())
            for (auto& [_, cb] : it->second) snapshot.push_back(cb);
        shadow[id] = value;
        broadcast_state(id, field, shadow[id]);
        return [snapshot = std::move(snapshot), value = std::move(value)] {
            notify_callbacks(snapshot, value);
        };
    }

    // Node handles keep destructors (including on_destroy and callback captures)
    // out of registry_mu, where they may reenter subscription cleanup.
    auto retire_view_locked(const std::string& id) {
        std::vector<decltype(commands)::node_type> retired_commands;
        retired_commands.reserve(commands.size());
        auto deactivate = [&](auto& callbacks) {
            if (auto found = callbacks.find(id); found != callbacks.end())
                for (auto& [_, node] : found->second)
                    node->active.store(false, std::memory_order_release);
        };
        deactivate(on_text); deactivate(on_bool); deactivate(on_int); deactivate(on_int64);
        deactivate(on_uint64); deactivate(on_float); deactivate(on_double); deactivate(on_click);
        for (auto it = commands.begin(); it != commands.end();) {
            if (it->first.first == id) {
                it->second->active.store(false, std::memory_order_release);
                retired_commands.push_back(commands.extract(it++));
            }
            else ++it;
        }
        shadow.erase(id);
        shadow_visible.erase(id);
        shadow_enabled.erase(id);
        return std::tuple{views.extract(id), on_text.extract(id), on_bool.extract(id),
            on_int.extract(id), on_int64.extract(id), on_uint64.extract(id),
            on_float.extract(id), on_double.extract(id), on_click.extract(id),
            std::move(retired_commands)};
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Route logic — each handler runs on the worker pool.
// ─────────────────────────────────────────────────────────────────────────────

HttpAdapter::Impl::RouteOutcome HttpAdapter::Impl::handle_health() {
    RouteOutcome out;
    out.body = json{{"ok", true}, {"protocol", wire::kProtocolVersion}}.dump();
    return out;
}

HttpAdapter::Impl::RouteOutcome HttpAdapter::Impl::handle_views() {
    RouteOutcome out;
    out.body = list_views_json().dump();
    return out;
}

HttpAdapter::Impl::RouteOutcome HttpAdapter::Impl::handle_get_state(std::string_view query) {
    std::string view_id = query_param(query, "view");
    if (view_id.empty()) return error_outcome(400, "missing view parameter");
    std::lock_guard<std::mutex> lock(registry_mu);
    auto it = views.find(view_id);
    if (it == views.end()) return error_outcome(404, "unknown view");
    RouteOutcome out;
    json body = {
        {"view",  view_id},
        {"kind",  std::string(it->second->kind())},
        {"value", wire_value(it->second->kind(), shadow.count(view_id) ? shadow[view_id] : json{})},
        {"visible", !shadow_visible.count(view_id) || shadow_visible[view_id]},
        {"enabled", !shadow_enabled.count(view_id) || shadow_enabled[view_id]},
    };
    out.body = body.dump();
    return out;
}

HttpAdapter::Impl::RouteOutcome HttpAdapter::Impl::handle_post_state(std::span<const std::byte> body_bytes) {
    const std::string raw(reinterpret_cast<const char*>(body_bytes.data()), body_bytes.size());
    bool drain = false;
    try {
        const auto body = json::parse(raw);
        if (!body.is_object() || !body.contains("view") || !body["view"].is_string() ||
            !body.contains("field") || !body["field"].is_string() || !body.contains("value")) {
            return error_outcome(400, "expected string view/field and a value");
        }
        auto id = body["view"].get<std::string>();
        auto field = body["field"].get<std::string>();
        const auto& value = body["value"];
        std::lock_guard<std::mutex> lock(registry_mu);
        auto view = views.find(id);
        if (view == views.end()) return error_outcome(404, "unknown view");
        if (view->second->kind() != field) {
            return error_outcome(400, "field does not match view kind");
        }
        if (notifications.size() >= config.max_pending_notifications) {
            return error_outcome(503, "notification capacity");
        }
        std::function<void()> notify;
        if (field == "text") notify = commit_state(id, field, value.get<std::string>(), on_text);
        else if (field == "bool") notify = commit_state(id, field, value.get<bool>(), on_bool);
        else if (field == "int") notify = commit_state(id, field, exact_integer<int>(value), on_int);
        else if (field == "int64") notify = commit_state(id, field, exact_integer<std::int64_t>(value, true), on_int64);
        else if (field == "uint64") notify = commit_state(id, field, exact_integer<std::uint64_t>(value, true), on_uint64);
        else if (field == "float") notify = commit_state(id, field, finite_number<float>(value), on_float);
        else if (field == "double") notify = commit_state(id, field, finite_number<double>(value), on_double);
        else return error_outcome(400, "unknown state field");
        drain = enqueue_notification(std::move(notify));
    } catch (const json::exception& e) {
        return error_outcome(400, std::string("invalid state: ") + e.what());
    } catch (const std::invalid_argument& e) {
        return error_outcome(400, e.what());
    }
    if (drain) drain_notifications();
    RouteOutcome out;
    out.body = R"({"ok":true})";
    return out;
}

HttpAdapter::Impl::RouteOutcome HttpAdapter::Impl::handle_post_click(std::span<const std::byte> body_bytes) {
    const std::string raw(reinterpret_cast<const char*>(body_bytes.data()), body_bytes.size());
    json body;
    try { body = json::parse(raw); }
    catch (const json::exception& e) {
        return error_outcome(400, std::string("bad json: ") + e.what());
    }
    if (!body.is_object() || !body.contains("view") || !body["view"].is_string()) {
        return error_outcome(400, "missing view");
    }
    std::string view_id = body["view"].get<std::string>();
    std::vector<decltype(on_click)::mapped_type::mapped_type> snap;
    bool drain = false;
    {
        std::lock_guard<std::mutex> lock(registry_mu);
        auto view = views.find(view_id);
        if (view == views.end()) return error_outcome(404, "unknown view");
        if (view->second->kind() != wire::field_kinds::kClick) {
            return error_outcome(400, "view is not a click view");
        }
        if (notifications.size() >= config.max_pending_notifications) {
            return error_outcome(503, "notification capacity");
        }
        auto it = on_click.find(view_id);
        if (it != on_click.end())
            for (auto& [_, cb] : it->second) snap.push_back(cb);
        broadcast_event(view_id, wire::field_kinds::kClick);
        drain = enqueue_notification([snapshot = std::move(snap)] {
            notify_callbacks(snapshot);
        });
    }
    if (drain) drain_notifications();
    RouteOutcome out;
    out.body = R"({"ok":true})";
    return out;
}

HttpAdapter::Impl::RouteOutcome HttpAdapter::Impl::handle_post_command(std::span<const std::byte> body_bytes) {
    const std::string raw(reinterpret_cast<const char*>(body_bytes.data()), body_bytes.size());
    json body;
    try { body = json::parse(raw); }
    catch (const json::exception& e) {
        return error_outcome(400, std::string("bad json: ") + e.what());
    }
    if (!body.is_object() ||
        !body.contains("view") || !body["view"].is_string() ||
        !body.contains("command") || !body["command"].is_string()) {
        return error_outcome(400, "missing view/command");
    }
    std::string view_id = body["view"].get<std::string>();
    std::string cmd_name = body["command"].get<std::string>();
    std::string args_json =
        body.contains("args") ? body["args"].dump() : std::string("{}");

    std::shared_ptr<CallbackNode<CommandHandler>> handler;
    {
        std::lock_guard<std::mutex> lock(registry_mu);
        if (!views.count(view_id)) return error_outcome(404, "unknown view");
        auto it = commands.find({view_id, cmd_name});
        if (it != commands.end()) handler = it->second;
    }
    if (!handler || !handler->active.load(std::memory_order_acquire)) {
        return error_outcome(404, "unknown command");
    }
    try {
        std::string resp = handler->callback(args_json);
        if (resp.empty()) resp = "{}";
        RouteOutcome out;
        out.body = std::move(resp);
        return out;
    } catch (...) {
        report_callback_failure("http.command", std::current_exception());
        return error_outcome(500, "command handler failed");
    }
}

HttpAdapter::Impl::RouteOutcome HttpAdapter::Impl::handle_static(
    const Mira::http::Request& request) {
    if (config.static_root.empty()) return error_outcome(404, "not found");

    // Decode, then refuse anything that escapes the configured root.
    std::string path = url_decode(split_target(request.target).path);
    if (path.empty() || path.front() != '/') return error_outcome(404, "not found");
    std::filesystem::path relative = path.substr(1);
    if (relative.empty()) relative = "index.html";
    if (relative.is_absolute()) return error_outcome(404, "not found");

    std::error_code fs_error;
    const std::filesystem::path root =
        std::filesystem::weakly_canonical(std::filesystem::path(config.static_root), fs_error);
    if (fs_error) return error_outcome(404, "not found");
    const std::filesystem::path resolved =
        std::filesystem::weakly_canonical(root / relative, fs_error);
    if (fs_error) return error_outcome(404, "not found");
    const auto root_text = root.generic_string();
    const auto resolved_text = resolved.generic_string();
    if (resolved_text != root_text &&
        resolved_text.rfind(root_text + '/', 0) != 0) {
        return error_outcome(404, "not found");
    }
    if (!std::filesystem::is_regular_file(resolved, fs_error) || fs_error) {
        return error_outcome(404, "not found");
    }

    std::ifstream file(resolved, std::ios::binary);
    if (!file) return error_outcome(404, "not found");
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    RouteOutcome out;
    out.content_type = std::string(static_content_type(resolved_text));
    out.body = std::move(content);
    return out;
}

HttpAdapter::Impl::RouteOutcome HttpAdapter::Impl::dispatch(
    const Mira::http::Request& request,
    std::span<const std::byte> body_bytes) {
    ExecutionScope scope(this);
    const auto [path, query] = split_target(request.target);
    const bool get_like = request.method == Mira::http::Method::get ||
                          request.method == Mira::http::Method::head;
    const std::string_view method_text = Mira::http::to_string(request.method);

    // CORS preflight: answered before routing, like the old global handler.
    if (request.method == Mira::http::Method::options) {
        RouteOutcome out;
        out.status = 204;
        out.content_type = "text/plain";
        return out;
    }

    const std::string& prefix = config.api_prefix;
    switch (classify(method_text, path, prefix)) {
    case Route::health:     return handle_health();
    case Route::views:      return handle_views();
    case Route::state_get:  return handle_get_state(query);
    case Route::state_post: return handle_post_state(body_bytes);
    case Route::click:      return handle_post_click(body_bytes);
    case Route::command:    return handle_post_command(body_bytes);
    case Route::stream:     // handled on the loop before dispatch runs
    case Route::other:      break;
    }

    // Outside the API prefix: static mount (GET/HEAD only), else 404.
    if (get_like && path.compare(0, prefix.size(), prefix) != 0) {
        return handle_static(request);
    }
    return error_outcome(404, "not found");
}

// ─────────────────────────────────────────────────────────────────────────────
// Request serving — coroutine starting on the loop thread.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

void apply_common_headers(Mira::http::Response& response,
                          const HttpAdapterConfig& config) {
    if (config.enable_cors) {
        response.headers.append("Access-Control-Allow-Origin", "*");
        response.headers.append("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        response.headers.append("Access-Control-Allow-Headers", "Content-Type");
    }
}

}  // namespace

template<class Stream>
Task<Mira::Result<void>> HttpAdapter::Impl::serve_request(
    const Mira::http::Request& request,
    Mira::http::ResponseWriter<Stream>& writer,
    std::span<const std::byte> body) {
    const auto [path, query] = split_target(request.target);
    const bool get_like = request.method == Mira::http::Method::get ||
                          request.method == Mira::http::Method::head;

    if (get_like &&
        classify(Mira::http::to_string(request.method), path, config.api_prefix) ==
            Route::stream) {
        co_return co_await run_sse(writer);
    }

    // Everything else: compute on the pool, write on the loop. The hop uses
    // Mira's resolver shape: this coroutine parks in `loop.sleep_until`
    // — which is loop-visible outstanding work, so the deadlock detector
    // never sees a fully suspended tree — and the pool job runs the
    // segment, then fires the stop token that resolves the sleep back on
    // the loop thread. No coroutine handle ever crosses a thread, nothing
    // self-references, and the gate dies with the request frame.
    struct PoolGate {
        std::exception_ptr error;
        std::optional<RouteOutcome> outcome;
        bool ran = false;
    };
    auto gate = std::make_shared<PoolGate>();
    auto hop_stop = std::make_shared<std::stop_source>();
    const std::stop_token hop_token = hop_stop->get_token();
    // Teardown may drop the pool job before it runs; the global stop then
    // has to resolve the sleep, or the connection would hang forever.
    std::stop_callback teardown_watch{stop_source.get_token(),
                                      [hop_stop] { hop_stop->request_stop(); }};
    // The job owns copies of its inputs: under callback-driven teardown this
    // job may outlive the request frame (and even the adapter), so nothing
    // here may reference the serving coroutine's stack.
    pool->post([this, gate, hop_stop, request,
                body = std::string(reinterpret_cast<const char*>(body.data()),
                                   body.size())]() mutable {
        try {
            gate->outcome = dispatch(request, {
                reinterpret_cast<const std::byte*>(body.data()), body.size()});
        } catch (...) {
            gate->error = std::current_exception();
        }
        gate->ran = true;
        hop_stop->request_stop();
    });
    co_await loop->sleep_until(EventLoop::Clock::time_point::max(),
                               {.stop = hop_token});
    // Back on the loop thread. request_stop is the job's last statement,
    // so the segment has fully run before this line — unless teardown
    // dropped the job and the global stop resolved the sleep instead.
    if (gate->error) std::rethrow_exception(gate->error);
    if (!gate->ran) co_return Mira::fail(Mira::Errc::cancelled);
    RouteOutcome outcome = std::move(*gate->outcome);

    Mira::http::Response response;
    response.status = outcome.status;
    response.headers.append("Content-Type", outcome.content_type);
    apply_common_headers(response, config);
    co_return co_await writer.send(response, as_bytes(outcome.body));
}

template<class Stream>
Task<Mira::Result<void>> HttpAdapter::Impl::run_sse(
    Mira::http::ResponseWriter<Stream>& writer) {
    auto client = std::make_shared<SseClient>(config.max_pending_sse_bytes);
    {
        // Identical order to a state commit: no live event can precede its snapshot.
        std::lock_guard<std::mutex> registry_lock(registry_mu);
        std::lock_guard<std::mutex> clients_lock(sse_mu);
        if (!running || sse_clients.size() >= sse_capacity) {
            Mira::http::Response rejection;
            rejection.status = 503;
            rejection.headers.append("Content-Type", "application/json");
            apply_common_headers(rejection, config);
            co_return co_await writer.send(
                rejection, as_bytes(json{{"error", "sse capacity"}}.dump()));
        }
        client->loop = &*loop;
        client->push(sse_frame(json{{"type", "hello"}, {"platform", "http"},
                                    {"protocol", wire::kProtocolVersion}}.dump()));
        for (auto& [id, view] : views) {
            auto kind = view->kind();
            if (kind != wire::field_kinds::kClick) {
                client->push(sse_frame(json{{"type", "state"}, {"view", id},
                    {"field", std::string(kind)},
                    {"value", wire_value(kind, shadow.count(id) ? shadow[id] : json{})}}.dump()));
            }
            client->push(sse_frame(json{{"type", "visibility"}, {"view", id},
                {"value", !shadow_visible.count(id) || shadow_visible[id]}}.dump()));
            client->push(sse_frame(json{{"type", "enabled"}, {"view", id},
                {"value", !shadow_enabled.count(id) || shadow_enabled[id]}}.dump()));
        }
        if (client->closed) {
            Mira::http::Response rejection;
            rejection.status = 503;
            rejection.headers.append("Content-Type", "application/json");
            apply_common_headers(rejection, config);
            co_return co_await writer.send(
                rejection,
                as_bytes(json{{"error",
                               "initial snapshot exceeds SSE buffer limit"}}.dump()));
        }
        sse_clients.push_back(client);
    }

    Mira::http::Response head;
    head.status = 200;
    head.headers.append("Content-Type", "text/event-stream");
    head.headers.append("Cache-Control", "no-cache");
    if (config.enable_cors) {
        head.headers.append("Access-Control-Allow-Origin", "*");
    }
    auto sent = co_await writer.send_head_chunked(head);
    if (!sent) {
        remove_sse_client(client);
        co_return Mira::fail(sent.error());
    }

    for (;;) {
        co_await SseWait{client.get()};
        std::deque<std::string> pending;
        bool done = false;
        {
            std::lock_guard<std::mutex> lock(client->mu);
            if (client->overflowed || client->closed) {
                done = true;
            } else {
                pending.swap(client->queue);
                client->queued_bytes = 0;
            }
        }
        if (done) break;
        while (!pending.empty()) {
            auto written = co_await writer.write(as_bytes(pending.front()));
            if (!written) {
                client->closed = true;
                remove_sse_client(client);
                co_return Mira::fail(written.error());
            }
            pending.pop_front();
        }
    }

    remove_sse_client(client);
    // Normal end: client closed or overflow-evicted. The last-chunk is the
    // honest termination either way; the socket closes with the connection.
    co_return co_await writer.finish();
}

// ─────────────────────────────────────────────────────────────────────────────
// Connection / accept loops — loop thread only.
// ─────────────────────────────────────────────────────────────────────────────

Task<void> HttpAdapter::Impl::connection_task_plain(Mira::transport::tcp::Socket socket) {
    Mira::http::ServerOptions options;
    options.stop = stop_source.get_token();
    options.max_requests_per_connection = 1'000'000;
    // No idle/request deadlines by default: the previous backend had none,
    // and a browser page holding one SSE stream must not be evicted. The
    // protocol limits still bound misbehaving peers.
    options.idle_timeout = EventLoop::Duration::zero();
    options.request_timeout = EventLoop::Duration::zero();

    auto handler = [this](const Mira::http::Request& request,
                          auto& writer,
                          std::span<const std::byte> body) -> Task<Result<void>> {
        co_return co_await serve_request(request, writer, body);
    };
    auto served = co_await Mira::http::serve_connection(socket, handler, options);
    // Connection-level failures (peer reset, parse errors, cancelled I/O)
    // end one connection; they are not server failures.
    (void)served;
    co_return;
}

#if defined(ARIA_HTTP_HAS_TLS)
Task<void> HttpAdapter::Impl::connection_task_tls(Mira::transport::tcp::Socket socket) {
    Mira::http::ServerOptions options;
    options.stop = stop_source.get_token();
    options.max_requests_per_connection = 1'000'000;
    options.idle_timeout = EventLoop::Duration::zero();
    options.request_timeout = EventLoop::Duration::zero();

    const OperationOptions io{.stop = options.stop};
    auto stream = Mira::tls::Stream<Mira::transport::tcp::Socket>::create(socket, *tls_context);
    if (!stream) co_return;
    auto handshake = co_await stream->handshake(io);
    if (!handshake) co_return;

    auto handler = [this](const Mira::http::Request& request,
                          auto& writer,
                          std::span<const std::byte> body) -> Task<Result<void>> {
        co_return co_await serve_request(request, writer, body);
    };
    auto served = co_await Mira::http::serve_connection(*stream, handler, options);
    (void)served;
    // Best-effort close_notify; the socket closes with the frame either way.
    (void)co_await stream->shutdown(io);
    co_return;
}
#endif

Task<void> HttpAdapter::Impl::accept_loop() {
    const std::stop_token stop = stop_source.get_token();
    Mira::TaskScope connections;
    for (;;) {
        auto accepted = co_await listener->accept(OperationOptions{.stop = stop});
        if (!accepted) {
            // Cancellation is the normal shutdown path; anything else means
            // the listener is no longer usable.
            break;
        }
#if defined(ARIA_HTTP_HAS_TLS)
        if (tls_active) {
            connections.spawn(connection_task_tls(std::move(*accepted)));
        } else {
            connections.spawn(connection_task_plain(std::move(*accepted)));
        }
#else
        connections.spawn(connection_task_plain(std::move(*accepted)));
#endif
    }
    // Drain: every connection coroutine unwinds (its I/O shares the stop
    // token) before this scope is destroyed.
    try {
        co_await connections.join();
    } catch (...) {
        report_callback_failure("http.connection", std::current_exception());
    }
    co_return;
}

Task<void> HttpAdapter::Impl::heartbeat_loop() {
    const std::stop_token stop = stop_source.get_token();
    for (;;) {
        auto slept = co_await loop->sleep_for(
            std::chrono::seconds(config.heartbeat_sec),
            OperationOptions{.stop = stop});
        if (!slept) co_return;  // cancelled by stop
        try {
            broadcast(json{{wire::fields::kType, wire::event_types::kPing}}.dump());
        } catch (...) {
            report_callback_failure("http.heartbeat", std::current_exception());
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

bool HttpAdapter::Impl::start_server() {
    if (executing == this)
        throw std::logic_error("HttpAdapter::start cannot run inside its HTTP task");
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mu);
    if (running) return false;
    stop_locked();  // Also reap a listener that exited unexpectedly.
    if (!tls_error.empty()) return false;  // TLS files rejected at construction

    // Numeric bind addresses only; "localhost" maps to the IPv4 loopback.
    auto address = config.host == "localhost"
        ? Mira::transport::Endpoint::loopback(config.port)
        : Mira::transport::Endpoint::parse(config.host, config.port);
    if (!address) return false;
    auto bound = Mira::transport::tcp::Listener::bind(*loop, *address);
    if (!bound) return false;
    listener = std::move(*bound);
    bound_port = listener->local_endpoint().port();

    stop_source = std::stop_source{};
    running = true;
    {
        // A fresh pool per start: the loop thread below joins these workers
        // before it exits, so every start/stop cycle owns exactly one pool.
        pool = std::make_shared<PoolState>();
        auto state = pool;
        for (std::size_t i = 0; i < pool_size; ++i) {
            workers.emplace_back([state] {
                for (;;) {
                    std::function<void()> job;
                    {
                        std::unique_lock<std::mutex> lock(state->mu);
                        state->cv.wait(lock,
                            [&] { return state->stopped || !state->jobs.empty(); });
                        if (state->jobs.empty()) return;  // stopped and drained
                        job = std::move(state->jobs.front());
                        state->jobs.pop_front();
                    }
                    job();
                }
            });
        }
    }

    auto self = shared_from_this();
    loop_thread = std::thread([self] {
        // Root task: accept loop + heartbeat, joined when everything unwinds.
        auto root = [self]() -> Task<void> {
            Mira::TaskScope scope;
            scope.spawn(self->accept_loop());
            scope.spawn(self->heartbeat_loop());
            try {
                co_await scope.join();
            } catch (...) {
                report_callback_failure("http.listener", std::current_exception());
            }
            co_return;
        };
        (void)self->loop->run_until_complete(root());
        // Join the pool before the keepalive self releases: workers never
        // outlive the loop, which is what makes their raw coroutine handles
        // safe. A callback-driven teardown detaches this thread instead and
        // the pool was already signalled from shutdown().
        self->pool->stop();
        for (auto& worker : self->workers) {
            if (worker.joinable()) worker.join();
        }
        self->running = false;
        self->bound_port = 0;
    });
    return true;
}

void HttpAdapter::Impl::stop_locked() {
    stop_source.request_stop();
    {
        std::lock_guard<std::mutex> lock(sse_mu);
        for (auto& client : sse_clients) client->close();
        sse_clients.clear();
    }
    // A callback-driven teardown releases the last Impl reference on the
    // loop thread itself; joining there would join the current thread. The
    // thread's own keepalive has already outlived every coroutine, so
    // detaching is the honest counterpart of the join on every other path.
    if (loop_thread.joinable()) {
        if (loop_thread.get_id() == std::this_thread::get_id()) {
            loop_thread.detach();
        } else {
            loop_thread.join();
        }
    }
    if (listener) {
        listener->close();
        listener.reset();
    }
    bound_port = 0;
}

void HttpAdapter::Impl::stop_server() {
    if (executing == this)
        throw std::logic_error("HttpAdapter::stop cannot run inside its HTTP task");
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mu);
    stop_locked();
}

void HttpAdapter::Impl::shutdown() noexcept {
    // Safe from any thread. Requesting the stop cancels every operation;
    // the loop thread then unwinds the accept loop, all connections, and
    // the pool before releasing its keepalive reference. Whoever drops the
    // last reference runs the destructor, and stop_locked() recognises the
    // loop-thread-calling-itself case there.
    stop_source.request_stop();
    pool->stop();
    {
        std::lock_guard<std::mutex> lock(sse_mu);
        for (auto& client : sse_clients) client->close();
        sse_clients.clear();
    }
    if (executing != this) {
        // Synchronous path: the caller waits for the loop thread to finish.
        if (loop_thread.joinable()) {
            if (loop_thread.get_id() == std::this_thread::get_id()) {
                loop_thread.detach();
            } else {
                loop_thread.join();
            }
        }
        if (listener) {
            listener->close();
            listener.reset();
        }
        bound_port = 0;
    }
    close_registry();
}

// ─────────────────────────────────────────────────────────────────────────────
// HttpAdapter — public methods
// ─────────────────────────────────────────────────────────────────────────────

HttpAdapter::HttpAdapter(HttpAdapterConfig config)
    : p_(std::make_shared<Impl>(std::move(config))) {}

HttpAdapter::~HttpAdapter() { p_->shutdown(); }

bool HttpAdapter::start() { return p_->start_server(); }
void HttpAdapter::stop()  { p_->stop_server(); }
bool HttpAdapter::running() const noexcept { return p_->running.load(); }

std::uint16_t HttpAdapter::actual_port() const noexcept {
    return p_->bound_port.load();
}
std::size_t HttpAdapter::client_count() const noexcept {
    std::lock_guard<std::mutex> lock(p_->sse_mu);
    return p_->sse_clients.size();
}

HttpView& HttpAdapter::register_view(std::string id, std::string kind) {
    auto owner = p_;
    auto next = std::make_shared<HttpView>(id, kind);
    auto* expected = next.get();
    {
        decltype(owner->retire_view_locked(id)) retired;
        {
            std::lock_guard lock(owner->registry_mu);
            if (!owner->registry_open)
                throw std::logic_error("HttpAdapter::register_view: adapter is closing");
            retired = owner->retire_view_locked(id);
            owner->views.emplace(id, next);
        }
        // Old view destruction may synchronously remove or replace the new
        // registration, or close the adapter. Never return a dangling view.
    }
    std::lock_guard lock(owner->registry_mu);
    auto found = owner->views.find(id);
    if (!owner->registry_open || found == owner->views.end() || found->second.get() != expected)
        throw std::logic_error("HttpAdapter::register_view: registration invalidated by destruction callback");
    return *expected;
}

HttpView* HttpAdapter::find_view(std::string_view id) {
    std::lock_guard<std::mutex> lock(p_->registry_mu);
    auto it = p_->views.find(std::string(id));
    return it == p_->views.end() ? nullptr : it->second.get();
}

void HttpAdapter::unregister_view(std::string_view id) {
    std::unique_lock<std::mutex> lock(p_->registry_mu);
    auto retired = p_->retire_view_locked(std::string(id));
    lock.unlock();
}

std::vector<HttpAdapter::ViewInfo> HttpAdapter::list_views() const {
    std::vector<ViewInfo> out;
    std::lock_guard<std::mutex> lock(p_->registry_mu);
    out.reserve(p_->views.size());
    for (auto& [id, view] : p_->views) {
        out.push_back({id, std::string(view->kind())});
    }
    return out;
}

// ── Text ───────────────────────────────────────────────────────────────────

void HttpAdapter::set_text(binding::IView& view, std::string_view text) {
    std::lock_guard lock(p_->registry_mu);
    auto* http = p_->owned_view_locked(view, "text");
    if (!http) return;
    p_->shadow[http->id()] = std::string(text);
    p_->broadcast_state(http->id(), "text", p_->shadow[http->id()]);
}

std::string HttpAdapter::get_text(binding::IView& view) {
    std::lock_guard lock(p_->registry_mu);
    auto* http = p_->owned_view_locked(view, "text");
    if (!http) return {};
    auto it = p_->shadow.find(http->id());
    return it != p_->shadow.end() && it->second.is_string() ? it->second.get<std::string>() : std::string{};
}

Subscription HttpAdapter::on_text_changed(binding::IView& view,
                                           std::function<void(std::string_view)> callback) {
    return p_->subscribe(view, "text", p_->on_text, std::move(callback));
}

#define ARIA_HTTP_SCALAR_IMPL(Type, Field)                                    \
    void HttpAdapter::set_##Field(binding::IView& view, Type value) {          \
        if constexpr (std::is_floating_point_v<Type>) {                       \
            if (!std::isfinite(value))                                       \
                throw std::invalid_argument("HTTP values must be finite");    \
        }                                                                    \
        std::lock_guard lock(p_->registry_mu);                                \
        auto* http = p_->owned_view_locked(view, #Field);                      \
        if (!http) return;                                                   \
        p_->shadow[http->id()] = value;                                       \
        p_->broadcast_state(http->id(), #Field, p_->shadow[http->id()]);        \
    }                                                                        \
    Type HttpAdapter::get_##Field(binding::IView& view) {                      \
        std::lock_guard lock(p_->registry_mu);                                \
        auto* http = p_->owned_view_locked(view, #Field);                      \
        if (!http) return Type{};                                            \
        auto it = p_->shadow.find(http->id());                                 \
        if (it == p_->shadow.end()) return Type{};                            \
        try { return it->second.get<Type>(); }                               \
        catch (...) { return Type{}; }                                       \
    }                                                                        \
    Subscription HttpAdapter::on_##Field##_changed(                           \
        binding::IView& view, std::function<void(Type)> callback) {            \
        return p_->subscribe(view, #Field, p_->on_##Field, std::move(callback)); \
    }

ARIA_HTTP_SCALAR_IMPL(bool, bool)
ARIA_HTTP_SCALAR_IMPL(int, int)
ARIA_HTTP_SCALAR_IMPL(std::int64_t, int64)
ARIA_HTTP_SCALAR_IMPL(std::uint64_t, uint64)
ARIA_HTTP_SCALAR_IMPL(float, float)
ARIA_HTTP_SCALAR_IMPL(double, double)
#undef ARIA_HTTP_SCALAR_IMPL

// ── Visibility / enabled ───────────────────────────────────────────────────

void HttpAdapter::set_visible(binding::IView& v, bool visible) {
    std::lock_guard<std::mutex> lock(p_->registry_mu);
    auto* view = p_->owned_view_locked(v);
    if (!view) return;
    auto& hv = *view;
    p_->shadow_visible[hv.id()] = visible;
    json env = {
        {wire::fields::kType,  wire::event_types::kVisibility},
        {wire::fields::kView,  hv.id()},
        {wire::fields::kValue, visible},
    };
    p_->broadcast(env.dump());
}

void HttpAdapter::set_enabled(binding::IView& v, bool enabled) {
    std::lock_guard<std::mutex> lock(p_->registry_mu);
    auto* view = p_->owned_view_locked(v);
    if (!view) return;
    auto& hv = *view;
    p_->shadow_enabled[hv.id()] = enabled;
    json env = {
        {wire::fields::kType,  wire::event_types::kEnabled},
        {wire::fields::kView,  hv.id()},
        {wire::fields::kValue, enabled},
    };
    p_->broadcast(env.dump());
}

// ── Click ──────────────────────────────────────────────────────────────────

Subscription HttpAdapter::on_click(binding::IView& view, std::function<void()> callback) {
    return p_->subscribe(view, "click", p_->on_click, std::move(callback));
}

// ── Custom command channel ────────────────────────────────────────────────

void HttpAdapter::register_command(std::string_view view_id,
                                    std::string_view command_name,
                                    CommandHandler handler) {
    auto next = std::make_shared<Impl::CallbackNode<CommandHandler>>(std::move(handler));
    std::shared_ptr<Impl::CallbackNode<CommandHandler>> retired;
    {
        std::lock_guard<std::mutex> lock(p_->registry_mu);
        if (!p_->registry_open) return;
        retired = std::exchange(p_->commands[{std::string(view_id), std::string(command_name)}],
                                std::move(next));
        if (retired) retired->active.store(false, std::memory_order_release);
    }
}

void HttpAdapter::unregister_command(std::string_view view_id,
                                     std::string_view command_name) {
    std::unique_lock<std::mutex> lock(p_->registry_mu);
    auto retired = p_->commands.extract({std::string(view_id), std::string(command_name)});
    if (retired) retired.mapped()->active.store(false, std::memory_order_release);
    lock.unlock();
}

}  // namespace aria::adapters::http

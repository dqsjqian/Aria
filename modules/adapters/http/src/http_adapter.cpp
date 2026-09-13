/// @file http_adapter.cpp
/// @brief HTTP/REST/SSE implementation built on cpp-httplib + nlohmann::json.
///
/// Both deps are vendored as single-header in third_party/. cpp-httplib
/// (Yuji Hirose, MIT) gives us the HTTP/1.1 server, routing, chunked
/// streaming for SSE, and worker thread pool. nlohmann::json (Niels
/// Lohmann, MIT) handles all JSON encode/decode.
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

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
#  include <openssl/ssl.h>
#endif

namespace aria::adapters::http {

using json = nlohmann::json;

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// SSE per-client outbox.
//
// cpp-httplib delivers chunked streams via a "content provider" callback
// that's invoked repeatedly until it returns false. We bridge that to a
// thread-safe queue so any thread (the binding engine, a Property setter,
// the heartbeat thread) can enqueue a frame without blocking on the
// network write loop.
// ─────────────────────────────────────────────────────────────────────────────

struct SseClient {
    explicit SseClient(std::size_t limit) : max_pending_bytes(limit) {}
    std::deque<std::string> queue;
    std::size_t queued_bytes = 0;
    const std::size_t max_pending_bytes;
    std::mutex mu;
    std::condition_variable cv;
    std::atomic<bool> closed{false};

    void push(std::string_view frame) {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (closed) return;
            if (frame.size() > max_pending_bytes - queued_bytes) {
                closed = true;
                queue.clear();
                queued_bytes = 0;
            } else {
                queue.emplace_back(frame);
                queued_bytes += frame.size();
            }
        }
        cv.notify_one();
    }

    void close() {
        {
            std::lock_guard<std::mutex> lk(mu);
            closed = true;
            queue.clear();
            queued_bytes = 0;
        }
        cv.notify_all();
    }
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
    class MarkedQueue final : public httplib::TaskQueue {
    public:
        MarkedQueue(Impl* owner, std::unique_ptr<httplib::TaskQueue> inner)
            : owner_(owner), inner_(std::move(inner)) {}
        bool enqueue(std::function<void()> fn) override {
            return inner_->enqueue([owner = owner_, fn = std::move(fn)] {
                ExecutionScope scope(owner);
                fn();
            });
        }
        void shutdown() override { inner_->shutdown(); }
        void on_idle() override { inner_->on_idle(); }
    private:
        Impl* owner_;
        std::unique_ptr<httplib::TaskQueue> inner_;
    };

    HttpAdapterConfig config;
    // svr is one of:
    //   - httplib::Server      (plain HTTP, always available)
    //   - httplib::SSLServer   (HTTPS, only when CPPHTTPLIB_OPENSSL_SUPPORT)
    // We hold them in a variant to keep a single code path for routing.
#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
    std::variant<std::monostate,
                 std::unique_ptr<httplib::Server>,
                 std::unique_ptr<httplib::SSLServer>> svr;
#else
    std::unique_ptr<httplib::Server> svr;
#endif
    std::thread server_thread;
    std::thread heartbeat_thread;
    std::atomic<bool> running{false};
    std::atomic<std::uint16_t> bound_port{0};
    bool tls_active = false;
    std::mutex lifecycle_mu;
    std::mutex heartbeat_mu;
    std::condition_variable heartbeat_cv;
    std::atomic<bool> listener_finished{true};
    std::atomic<bool> pool_ready{false};
    std::function<httplib::TaskQueue*()> saved_queue_factory;
    std::size_t workers = 0;
    std::size_t sse_capacity = 0;

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

    explicit Impl(HttpAdapterConfig c) : config(std::move(c)) {
        if (config.worker_threads < 0 || config.worker_threads == 1 ||
            config.heartbeat_sec <= 0 || config.max_sse_clients < 0 ||
            config.max_pending_sse_bytes == 0 || config.max_pending_notifications == 0)
            throw std::invalid_argument("invalid HTTP worker, heartbeat or SSE limit");
        workers = config.worker_threads == 0
            ? std::max(2u, std::thread::hardware_concurrency())
            : static_cast<std::size_t>(config.worker_threads);
        sse_capacity = config.max_sse_clients == 0 ? workers - 1
            : std::min(workers - 1, static_cast<std::size_t>(config.max_sse_clients));
        const bool cert = !config.tls_cert_file.empty();
        const bool key = !config.tls_key_file.empty();
        if (cert != key || (!config.tls_ca_file.empty() && !cert))
            throw std::invalid_argument("TLS requires both certificate and private key");
        if (config.tls_min_version != "1.2" && config.tls_min_version != "1.3")
            throw std::invalid_argument("TLS minimum version must be 1.2 or 1.3");
        construct_server();
        server().new_task_queue = [count = workers] {
            return new httplib::ThreadPool(count, count);
        };
        register_routes();
    }

    ~Impl() {
        stop_server();
        close_registry();
    }

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

    // ── Server construction (HTTP vs HTTPS) ────────────────────────────

    void construct_server() {
        bool want_tls = !config.tls_cert_file.empty() &&
                        !config.tls_key_file.empty();
#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
        if (want_tls) {
            const char* ca = config.tls_ca_file.empty()
                                 ? nullptr
                                 : config.tls_ca_file.c_str();
            auto s = std::make_unique<httplib::SSLServer>(
                config.tls_cert_file.c_str(),
                config.tls_key_file.c_str(),
                ca);

            // Enforce minimum TLS version on the underlying SSL_CTX.
            // cpp-httplib >= 0.19 renamed ssl_context() to tls_context()
            // and made it backend-agnostic (tls::ctx_t = void*); with the
            // OpenSSL backend the handle is still an SSL_CTX*.
            auto* ctx = static_cast<SSL_CTX*>(s->tls_context());
            if (ctx) {
                int min_ver = TLS1_2_VERSION;
                if (config.tls_min_version == "1.3") min_ver = TLS1_3_VERSION;
                SSL_CTX_set_min_proto_version(ctx, min_ver);
                SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
                if (ca) {
                    SSL_CTX_set_verify(ctx,
                        SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                        nullptr);
                }
            }

            tls_active = true;
            svr = std::move(s);
            return;
        }
#else
        if (want_tls)
            throw std::invalid_argument("TLS configured but this HTTP adapter was built without TLS support");
#endif
        auto s = std::make_unique<httplib::Server>();
        tls_active = false;
#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
        svr = std::move(s);
#else
        svr = std::move(s);
#endif
    }

    // Type-erased accessor over the active server (plain or SSL).
    httplib::Server& server() {
#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
        if (auto* p = std::get_if<std::unique_ptr<httplib::Server>>(&svr))
            return **p;
        if (auto* p = std::get_if<std::unique_ptr<httplib::SSLServer>>(&svr))
            return **p;
        std::abort();  // unreachable: construct_server always sets one
#else
        return *svr;
#endif
    }

    // ── Routing ────────────────────────────────────────────────────────

    void register_routes() {
        const std::string& prefix = config.api_prefix;

        // CORS preflight.
        if (config.enable_cors) {
            server().set_default_headers({
                {"Access-Control-Allow-Origin",  "*"},
                {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
                {"Access-Control-Allow-Headers", "Content-Type"},
            });
            server().Options(".*",
                [](const httplib::Request&, httplib::Response& res) {
                    res.status = 204;
                });
        }

        server().Get(prefix + "/health",
            [](const httplib::Request&, httplib::Response& res) {
                res.set_content(json{{"ok", true}, {"protocol", wire::kProtocolVersion}}.dump(),
                                "application/json");
            });

        server().Get(prefix + "/views",
            [this](const httplib::Request&, httplib::Response& res) {
                res.set_content(list_views_json().dump(),
                                "application/json");
            });

        server().Get(prefix + "/state",
            [this](const httplib::Request& req, httplib::Response& res) {
                handle_get_state(req, res);
            });

        server().Post(prefix + "/state",
            [this](const httplib::Request& req, httplib::Response& res) {
                handle_post_state(req, res);
            });

        server().Post(prefix + "/click",
            [this](const httplib::Request& req, httplib::Response& res) {
                handle_post_click(req, res);
            });

        server().Post(prefix + "/command",
            [this](const httplib::Request& req, httplib::Response& res) {
                handle_post_command(req, res);
            });

        server().Get(prefix + "/stream",
            [this](const httplib::Request& req, httplib::Response& res) {
                handle_sse(req, res);
            });

        if (!config.static_root.empty()) {
            server().set_mount_point("/", config.static_root);
        }
    }

    // ── Helpers ────────────────────────────────────────────────────────

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
            std::lock_guard<std::mutex> lk(sse_mu);
            snapshot = sse_clients;
        }
        for (auto& c : snapshot) {
            if (!c->closed) c->push(frame);
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
        std::lock_guard<std::mutex> lk(registry_mu);
        json arr = json::array();
        for (auto& [id, view] : views) {
            arr.push_back({
                {"id",   id},
                {"kind", std::string(view->kind())},
            });
        }
        return {{"views", arr}};
    }

    static void send_error(httplib::Response& res, int code,
                           const std::string& message) {
        res.status = code;
        res.set_content(json{{"error", message}}.dump(), "application/json");
    }

    // ── Handlers ───────────────────────────────────────────────────────

    void handle_get_state(const httplib::Request& req,
                          httplib::Response& res) {
        auto view_id = req.get_param_value("view");
        if (view_id.empty()) {
            send_error(res, 400, "missing view parameter");
            return;
        }
        std::lock_guard<std::mutex> lk(registry_mu);
        auto it = views.find(view_id);
        if (it == views.end()) {
            send_error(res, 404, "unknown view");
            return;
        }
        json out = {
            {"view",  view_id},
            {"kind",  std::string(it->second->kind())},
            {"value", wire_value(it->second->kind(), shadow.count(view_id) ? shadow[view_id] : json{})},
            {"visible", !shadow_visible.count(view_id) || shadow_visible[view_id]},
            {"enabled", !shadow_enabled.count(view_id) || shadow_enabled[view_id]},
        };
        res.set_content(out.dump(), "application/json");
    }

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

    void handle_post_state(const httplib::Request& req,
                           httplib::Response& res) {
        bool drain = false;
        try {
            const auto body = json::parse(req.body);
            if (!body.is_object() || !body.contains("view") || !body["view"].is_string() ||
                !body.contains("field") || !body["field"].is_string() || !body.contains("value")) {
                send_error(res, 400, "expected string view/field and a value");
                return;
            }
            auto id = body["view"].get<std::string>();
            auto field = body["field"].get<std::string>();
            const auto& value = body["value"];
            std::lock_guard<std::mutex> lk(registry_mu);
            auto view = views.find(id);
            if (view == views.end()) {
                send_error(res, 404, "unknown view");
                return;
            }
            if (view->second->kind() != field) {
                send_error(res, 400, "field does not match view kind");
                return;
            }
            if (notifications.size() >= config.max_pending_notifications) {
                send_error(res, 503, "notification capacity");
                return;
            }
            std::function<void()> notify;
            if (field == "text") notify = commit_state(id, field, value.get<std::string>(), on_text);
            else if (field == "bool") notify = commit_state(id, field, value.get<bool>(), on_bool);
            else if (field == "int") notify = commit_state(id, field, exact_integer<int>(value), on_int);
            else if (field == "int64") notify = commit_state(id, field, exact_integer<std::int64_t>(value, true), on_int64);
            else if (field == "uint64") notify = commit_state(id, field, exact_integer<std::uint64_t>(value, true), on_uint64);
            else if (field == "float") notify = commit_state(id, field, finite_number<float>(value), on_float);
            else if (field == "double") notify = commit_state(id, field, finite_number<double>(value), on_double);
            else { send_error(res, 400, "unknown state field"); return; }
            drain = enqueue_notification(std::move(notify));
        } catch (const json::exception& e) {
            send_error(res, 400, std::string("invalid state: ") + e.what());
            return;
        } catch (const std::invalid_argument& e) {
            send_error(res, 400, e.what());
            return;
        }
        if (drain) drain_notifications();
        res.set_content(R"({"ok":true})", "application/json");
    }

    void handle_post_click(const httplib::Request& req,
                           httplib::Response& res) {
        json body;
        try { body = json::parse(req.body); }
        catch (const json::exception& e) {
            send_error(res, 400, std::string("bad json: ") + e.what());
            return;
        }
        if (!body.is_object() || !body.contains("view") || !body["view"].is_string()) {
            send_error(res, 400, "missing view");
            return;
        }
        std::string view_id = body["view"].get<std::string>();
        std::vector<decltype(on_click)::mapped_type::mapped_type> snap;
        bool drain = false;
        {
            std::lock_guard<std::mutex> lk(registry_mu);
            auto view = views.find(view_id);
            if (view == views.end()) { send_error(res, 404, "unknown view"); return; }
            if (view->second->kind() != wire::field_kinds::kClick) {
                send_error(res, 400, "view is not a click view"); return;
            }
            if (notifications.size() >= config.max_pending_notifications) {
                send_error(res, 503, "notification capacity");
                return;
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
        res.set_content(R"({"ok":true})", "application/json");
    }

    void handle_post_command(const httplib::Request& req,
                             httplib::Response& res) {
        json body;
        try { body = json::parse(req.body); }
        catch (const json::exception& e) {
            send_error(res, 400, std::string("bad json: ") + e.what());
            return;
        }
        if (!body.is_object() ||
            !body.contains("view") || !body["view"].is_string() ||
            !body.contains("command") || !body["command"].is_string()) {
            send_error(res, 400, "missing view/command");
            return;
        }
        std::string view_id = body["view"].get<std::string>();
        std::string cmd_name = body["command"].get<std::string>();
        std::string args_json =
            body.contains("args") ? body["args"].dump() : std::string("{}");

        std::shared_ptr<CallbackNode<CommandHandler>> handler;
        {
            std::lock_guard<std::mutex> lk(registry_mu);
            if (!views.count(view_id)) { send_error(res, 404, "unknown view"); return; }
            auto it = commands.find({view_id, cmd_name});
            if (it != commands.end()) handler = it->second;
        }
        if (!handler || !handler->active.load(std::memory_order_acquire)) {
            send_error(res, 404, "unknown command");
            return;
        }
        try {
            std::string resp = handler->callback(args_json);
            if (resp.empty()) resp = "{}";
            res.set_content(resp, "application/json");
        } catch (...) {
            report_callback_failure("http.command", std::current_exception());
            send_error(res, 500, "command handler failed");
        }
    }

    void handle_sse(const httplib::Request&, httplib::Response& res) {
        auto client = std::make_shared<SseClient>(config.max_pending_sse_bytes);
        {
            // Identical order to a state commit: no live event can precede its snapshot.
            std::lock_guard<std::mutex> registry_lock(registry_mu);
            std::lock_guard<std::mutex> clients_lock(sse_mu);
            if (!running || sse_clients.size() >= sse_capacity) {
                send_error(res, 503, "sse capacity");
                return;
            }
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
                send_error(res, 503, "initial snapshot exceeds SSE buffer limit");
                return;
            }
            sse_clients.push_back(client);
        }

        // SSE-specific headers.
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection",    "keep-alive");
        if (config.enable_cors) {
            res.set_header("Access-Control-Allow-Origin", "*");
        }

        // Capture by value so the lambda owns its share of the client
        // while the listener retains Impl until its worker pool has joined.
        Impl* impl = this;
        auto provider = [impl, client](std::size_t /*offset*/,
                                       httplib::DataSink& sink) -> bool {
            // Pull all pending frames; block briefly between flushes so
            // we don't busy-spin when idle.
            std::unique_lock<std::mutex> lk(client->mu);
            client->cv.wait_for(lk, std::chrono::seconds(1), [&] {
                return !client->queue.empty() || client->closed ||
                       !impl->running.load();
            });
            if (client->closed || !impl->running.load()) {
                sink.done();
                return true;
            }
            while (!client->queue.empty()) {
                std::string frame = std::move(client->queue.front());
                client->queue.pop_front();
                client->queued_bytes -= frame.size();
                lk.unlock();
                if (!sink.write(frame.data(), frame.size())) {
                    client->closed = true;
                    lk.lock();
                    sink.done();
                    return true;
                }
                lk.lock();
            }
            return true;
        };

        auto on_complete = [this, client](bool /*success*/) {
            std::lock_guard<std::mutex> lk(sse_mu);
            sse_clients.erase(
                std::remove(sse_clients.begin(), sse_clients.end(), client),
                sse_clients.end());
        };

        res.set_chunked_content_provider("text/event-stream",
                                         std::move(provider),
                                         std::move(on_complete));
    }

    // ── Lifecycle ──────────────────────────────────────────────────────

    bool start_server() {
        if (executing == this)
            throw std::logic_error("HttpAdapter::start cannot run inside its HTTP task");
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mu);
        if (running) return false;
        stop_locked(); // Also reap a listener that exited unexpectedly.
        std::uint16_t resolved_port = config.port;
        if (!server().is_valid()) return false;
        if (config.port == 0) {
            int picked = server().bind_to_any_port(config.host);
            if (picked < 0) return false;
            resolved_port = static_cast<std::uint16_t>(picked);
        } else if (!server().bind_to_port(config.host, config.port)) return false;

        bound_port = resolved_port;
        running = true;
        listener_finished = false;
        pool_ready = false;
        try {
            saved_queue_factory = std::move(server().new_task_queue);
            server().new_task_queue = [this, factory = saved_queue_factory] {
                std::unique_ptr<httplib::TaskQueue> queue(factory());
                if (!queue) throw std::runtime_error("HTTP task queue factory returned null");
                auto marked = std::make_unique<MarkedQueue>(this, std::move(queue));
                pool_ready = true;
                return marked.release();
            };
            server_thread = std::thread([self = shared_from_this()] {
                ExecutionScope scope(self.get());
                try { self->server().listen_after_bind(); }
                catch (...) { report_callback_failure("http.listener", std::current_exception()); }
                self->running = false;
                self->bound_port = 0;
                self->listener_finished = true;
                self->heartbeat_cv.notify_all();
            });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!pool_ready && !listener_finished &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (listener_finished || !pool_ready) { stop_locked(); return false; }
            heartbeat_thread = std::thread([self = shared_from_this()] {
                ExecutionScope scope(self.get());
                std::unique_lock<std::mutex> lk(self->heartbeat_mu);
                while (!self->heartbeat_cv.wait_for(lk, std::chrono::seconds(self->config.heartbeat_sec),
                                               [&] { return !self->running.load(); })) {
                    lk.unlock();
                    try { self->broadcast(json{{wire::fields::kType, wire::event_types::kPing}}.dump()); }
                    catch (...) { report_callback_failure("http.heartbeat", std::current_exception()); }
                    lk.lock();
                }
            });
        } catch (...) { stop_locked(); return false; }
        return true;
    }

    void request_stop_locked() {
        {
            std::lock_guard<std::mutex> lk(heartbeat_mu);
            running = false;
        }
        heartbeat_cv.notify_all();
        server().stop();
        {
            std::lock_guard<std::mutex> lk(sse_mu);
            for (auto& c : sse_clients) c->close();
            sse_clients.clear();
        }
    }

    void stop_locked() {
        request_stop_locked();
        if (server_thread.joinable()) server_thread.join();
        if (heartbeat_thread.joinable()) heartbeat_thread.join();
        if (saved_queue_factory) server().new_task_queue = std::move(saved_queue_factory);
        pool_ready = false;
        bound_port = 0;
    }

    void stop_server() {
        if (executing == this)
            throw std::logic_error("HttpAdapter::stop cannot run inside its HTTP task");
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mu);
        stop_locked();
    }

    void shutdown() noexcept {
        if (executing == this) {
            // listen_after_bind does not return until its task queue has joined
            // every worker. Both background functions retain shared Impl, so
            // detaching their handles here preserves all native state until
            // the current callback and the listener have actually returned.
            std::lock_guard lock(lifecycle_mu);
            request_stop_locked();
            if (server_thread.joinable()) server_thread.detach();
            if (heartbeat_thread.joinable()) heartbeat_thread.detach();
            bound_port = 0;
        } else {
            stop_server();
        }
        close_registry();
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
// HttpAdapter — public methods
// ─────────────────────────────────────────────────────────────────────────────

HttpAdapter::HttpAdapter(HttpAdapterConfig config)
    : p_(std::make_shared<Impl>(std::move(config))) {}

HttpAdapter::~HttpAdapter() { p_->shutdown(); }

bool HttpAdapter::start() { return p_->start_server(); }
void HttpAdapter::stop()  { p_->stop_server(); }
bool HttpAdapter::running() const noexcept { return p_->running.load(); }

::httplib::Server& HttpAdapter::native_server() noexcept {
    // Impl::server() always returns a valid reference because
    // construct_server() runs eagerly in Impl's constructor and
    // unconditionally installs either an httplib::Server or
    // httplib::SSLServer. SSLServer derives from Server, so the
    // reference is well-formed in both modes.
    return p_->server();
}
std::uint16_t HttpAdapter::actual_port() const noexcept {
    return p_->bound_port.load();
}
std::size_t HttpAdapter::client_count() const noexcept {
    std::lock_guard<std::mutex> lk(p_->sse_mu);
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
    std::lock_guard<std::mutex> lk(p_->registry_mu);
    auto it = p_->views.find(std::string(id));
    return it == p_->views.end() ? nullptr : it->second.get();
}

void HttpAdapter::unregister_view(std::string_view id) {
    std::unique_lock<std::mutex> lk(p_->registry_mu);
    auto retired = p_->retire_view_locked(std::string(id));
    lk.unlock();
}

std::vector<HttpAdapter::ViewInfo> HttpAdapter::list_views() const {
    std::vector<ViewInfo> out;
    std::lock_guard<std::mutex> lk(p_->registry_mu);
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
    std::lock_guard<std::mutex> lk(p_->registry_mu);
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
    std::lock_guard<std::mutex> lk(p_->registry_mu);
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
        std::lock_guard<std::mutex> lk(p_->registry_mu);
        if (!p_->registry_open) return;
        retired = std::exchange(p_->commands[{std::string(view_id), std::string(command_name)}],
                                std::move(next));
        if (retired) retired->active.store(false, std::memory_order_release);
    }
}

void HttpAdapter::unregister_command(std::string_view view_id,
                                      std::string_view command_name) {
    std::unique_lock<std::mutex> lk(p_->registry_mu);
    auto retired = p_->commands.extract({std::string(view_id), std::string(command_name)});
    if (retired) retired.mapped()->active.store(false, std::memory_order_release);
    lk.unlock();
}

}  // namespace aria::adapters::http

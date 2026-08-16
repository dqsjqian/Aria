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

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
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
    std::deque<std::string> queue;
    std::mutex mu;
    std::condition_variable cv;
    std::atomic<bool> closed{false};

    void push(std::string frame) {
        {
            std::lock_guard<std::mutex> lk(mu);
            queue.push_back(std::move(frame));
        }
        cv.notify_one();
    }

    void close() {
        closed = true;
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

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// HttpAdapter::Impl
// ─────────────────────────────────────────────────────────────────────────────

struct HttpAdapter::Impl {
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

    // View registry.
    mutable std::mutex registry_mu;
    std::unordered_map<std::string, std::unique_ptr<HttpView>> views;
    std::unordered_map<std::string, json> shadow;
    std::unordered_map<std::string, bool> shadow_visible;
    std::unordered_map<std::string, bool> shadow_enabled;

    // Subscriptions, keyed by view_id then by stable subscription id.
    using TextCb   = std::function<void(std::string_view)>;
    using BoolCb   = std::function<void(bool)>;
    using IntCb    = std::function<void(int)>;
    using Int64Cb  = std::function<void(std::int64_t)>;
    using UInt64Cb = std::function<void(std::uint64_t)>;
    using FloatCb  = std::function<void(float)>;
    using DoubleCb = std::function<void(double)>;
    using ClickCb  = std::function<void()>;

    std::unordered_map<std::string, std::map<std::uint64_t, TextCb>>   on_text;
    std::unordered_map<std::string, std::map<std::uint64_t, BoolCb>>   on_bool;
    std::unordered_map<std::string, std::map<std::uint64_t, IntCb>>    on_int;
    std::unordered_map<std::string, std::map<std::uint64_t, Int64Cb>>  on_int64;
    std::unordered_map<std::string, std::map<std::uint64_t, UInt64Cb>> on_uint64;
    std::unordered_map<std::string, std::map<std::uint64_t, FloatCb>>  on_float;
    std::unordered_map<std::string, std::map<std::uint64_t, DoubleCb>> on_double;
    std::unordered_map<std::string, std::map<std::uint64_t, ClickCb>>  on_click;
    std::atomic<std::uint64_t> next_sub_id{1};

    std::map<std::pair<std::string, std::string>, CommandHandler> commands;

    mutable std::mutex sse_mu;
    std::vector<std::shared_ptr<SseClient>> sse_clients;

    explicit Impl(HttpAdapterConfig c) : config(std::move(c)) {
        construct_server();
        register_routes();
    }

    ~Impl() { stop_server(); }

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
        if (want_tls) {
            // TLS requested but adapter built without OpenSSL — fall
            // back to plain HTTP and emit a one-time stderr note.
            std::fprintf(stderr,
                "[aria::http] WARNING: TLS cert/key configured but adapter "
                "built without TLS support (set ARIA_HTTP_ENABLE_TLS=ON). "
                "Falling back to plain HTTP.\n");
        }
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
                res.set_content(R"({"ok":true,"protocol":1})",
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
            {wire::fields::kValue, value},
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
            {"value", shadow.count(view_id) ? shadow[view_id] : json{}},
        };
        res.set_content(out.dump(), "application/json");
    }

    void handle_post_state(const httplib::Request& req,
                           httplib::Response& res) {
        json body;
        try { body = json::parse(req.body); }
        catch (const json::parse_error& e) {
            send_error(res, 400, std::string("bad json: ") + e.what());
            return;
        }
        if (!body.is_object() ||
            !body.contains("view") || !body.contains("field") ||
            !body.contains("value")) {
            send_error(res, 400, "missing view/field/value");
            return;
        }
        std::string view_id = body["view"].get<std::string>();
        std::string field = body["field"].get<std::string>();
        json value = body["value"];

        try {
            if (field == wire::field_kinds::kText) {
                fire_text(view_id, value.get<std::string>());
            } else if (field == wire::field_kinds::kBool) {
                fire_bool(view_id, value.get<bool>());
            } else if (field == wire::field_kinds::kInt) {
                fire_int(view_id, value.get<int>());
            } else if (field == wire::field_kinds::kInt64) {
                fire_int64(view_id, value.get<std::int64_t>());
            } else if (field == wire::field_kinds::kUInt64) {
                fire_uint64(view_id, value.get<std::uint64_t>());
            } else if (field == wire::field_kinds::kFloat) {
                fire_float(view_id, value.get<float>());
            } else if (field == wire::field_kinds::kDouble) {
                fire_double(view_id, value.get<double>());
            } else {
                send_error(res, 400, "unknown field kind");
                return;
            }
        } catch (const json::type_error& e) {
            send_error(res, 400, std::string("type mismatch: ") + e.what());
            return;
        }
        res.set_content(R"({"ok":true})", "application/json");
    }

    void handle_post_click(const httplib::Request& req,
                           httplib::Response& res) {
        json body;
        try { body = json::parse(req.body); }
        catch (const json::parse_error& e) {
            send_error(res, 400, std::string("bad json: ") + e.what());
            return;
        }
        if (!body.is_object() || !body.contains("view")) {
            send_error(res, 400, "missing view");
            return;
        }
        std::string view_id = body["view"].get<std::string>();
        std::vector<ClickCb> snap;
        {
            std::lock_guard<std::mutex> lk(registry_mu);
            auto it = on_click.find(view_id);
            if (it != on_click.end())
                for (auto& [_, cb] : it->second) snap.push_back(cb);
        }
        for (auto& cb : snap) cb();
        broadcast_event(view_id, wire::field_kinds::kClick);
        res.set_content(R"({"ok":true})", "application/json");
    }

    void handle_post_command(const httplib::Request& req,
                             httplib::Response& res) {
        json body;
        try { body = json::parse(req.body); }
        catch (const json::parse_error& e) {
            send_error(res, 400, std::string("bad json: ") + e.what());
            return;
        }
        if (!body.is_object() ||
            !body.contains("view") || !body.contains("command")) {
            send_error(res, 400, "missing view/command");
            return;
        }
        std::string view_id = body["view"].get<std::string>();
        std::string cmd_name = body["command"].get<std::string>();
        std::string args_json =
            body.contains("args") ? body["args"].dump() : std::string("{}");

        CommandHandler handler;
        {
            std::lock_guard<std::mutex> lk(registry_mu);
            auto it = commands.find({view_id, cmd_name});
            if (it != commands.end()) handler = it->second;
        }
        if (!handler) {
            send_error(res, 404, "unknown command");
            return;
        }
        std::string resp = handler(args_json);
        if (resp.empty()) resp = "{}";
        res.set_content(resp, "application/json");
    }

    void handle_sse(const httplib::Request&, httplib::Response& res) {
        // Capacity check.
        {
            std::lock_guard<std::mutex> lk(sse_mu);
            if (config.max_sse_clients > 0 &&
                static_cast<int>(sse_clients.size()) >= config.max_sse_clients) {
                send_error(res, 503, "sse capacity");
                return;
            }
        }

        auto client = std::make_shared<SseClient>();
        {
            std::lock_guard<std::mutex> lk(sse_mu);
            sse_clients.push_back(client);
        }

        // Seed the queue with hello + initial snapshots BEFORE the
        // streaming loop starts, so the first chunked write delivers
        // them in one shot.
        json hello = {
            {wire::fields::kType, wire::event_types::kHello},
            {"platform", "http"},
            {"protocol", wire::kProtocolVersion},
        };
        client->push(sse_frame(hello.dump()));

        {
            std::lock_guard<std::mutex> lk(registry_mu);
            for (auto& [id, view] : views) {
                std::string_view k = view->kind();
                if (k == wire::field_kinds::kClick) continue;
                json env = {
                    {wire::fields::kType,  wire::event_types::kState},
                    {wire::fields::kView,  id},
                    {wire::fields::kField, std::string(k)},
                    {wire::fields::kValue,
                        shadow.count(id) ? shadow[id] : json{}},
                };
                client->push(sse_frame(env.dump()));
            }
        }

        // SSE-specific headers.
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection",    "keep-alive");
        if (config.enable_cors) {
            res.set_header("Access-Control-Allow-Origin", "*");
        }

        // Capture by value so the lambda owns its share of the client
        // and the impl pointer survives the response lifetime.
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

    // ── Shadow-state mutators (post-state path) ────────────────────────

    void fire_text(const std::string& view_id, const std::string& s) {
        std::vector<TextCb> snap;
        {
            std::lock_guard<std::mutex> lk(registry_mu);
            shadow[view_id] = s;
            auto it = on_text.find(view_id);
            if (it != on_text.end())
                for (auto& [_, cb] : it->second) snap.push_back(cb);
        }
        for (auto& cb : snap) cb(s);
        broadcast_state(view_id, wire::field_kinds::kText, json(s));
    }

    void fire_bool(const std::string& view_id, bool b) {
        std::vector<BoolCb> snap;
        {
            std::lock_guard<std::mutex> lk(registry_mu);
            shadow[view_id] = b;
            auto it = on_bool.find(view_id);
            if (it != on_bool.end())
                for (auto& [_, cb] : it->second) snap.push_back(cb);
        }
        for (auto& cb : snap) cb(b);
        broadcast_state(view_id, wire::field_kinds::kBool, json(b));
    }

    void fire_int(const std::string& view_id, int i) {
        std::vector<IntCb> snap;
        {
            std::lock_guard<std::mutex> lk(registry_mu);
            shadow[view_id] = i;
            auto it = on_int.find(view_id);
            if (it != on_int.end())
                for (auto& [_, cb] : it->second) snap.push_back(cb);
        }
        for (auto& cb : snap) cb(i);
        broadcast_state(view_id, wire::field_kinds::kInt, json(i));
    }

    void fire_int64(const std::string& view_id, std::int64_t x) {
        std::vector<Int64Cb> snap;
        {
            std::lock_guard<std::mutex> lk(registry_mu);
            shadow[view_id] = x;
            auto it = on_int64.find(view_id);
            if (it != on_int64.end())
                for (auto& [_, cb] : it->second) snap.push_back(cb);
        }
        for (auto& cb : snap) cb(x);
        broadcast_state(view_id, wire::field_kinds::kInt64, json(x));
    }

    void fire_uint64(const std::string& view_id, std::uint64_t x) {
        std::vector<UInt64Cb> snap;
        {
            std::lock_guard<std::mutex> lk(registry_mu);
            shadow[view_id] = x;
            auto it = on_uint64.find(view_id);
            if (it != on_uint64.end())
                for (auto& [_, cb] : it->second) snap.push_back(cb);
        }
        for (auto& cb : snap) cb(x);
        broadcast_state(view_id, wire::field_kinds::kUInt64, json(x));
    }

    void fire_float(const std::string& view_id, float f) {
        std::vector<FloatCb> snap;
        {
            std::lock_guard<std::mutex> lk(registry_mu);
            shadow[view_id] = static_cast<double>(f);
            auto it = on_float.find(view_id);
            if (it != on_float.end())
                for (auto& [_, cb] : it->second) snap.push_back(cb);
        }
        for (auto& cb : snap) cb(f);
        broadcast_state(view_id, wire::field_kinds::kFloat,
                        json(static_cast<double>(f)));
    }

    void fire_double(const std::string& view_id, double d) {
        std::vector<DoubleCb> snap;
        {
            std::lock_guard<std::mutex> lk(registry_mu);
            shadow[view_id] = d;
            auto it = on_double.find(view_id);
            if (it != on_double.end())
                for (auto& [_, cb] : it->second) snap.push_back(cb);
        }
        for (auto& cb : snap) cb(d);
        broadcast_state(view_id, wire::field_kinds::kDouble, json(d));
    }

    // ── Lifecycle ──────────────────────────────────────────────────────

    bool start_server() {
        if (running.exchange(true)) return false;

        // cpp-httplib has two bind APIs with different return types:
        //   * bind_to_port(host, port)  -> bool   (success/fail)
        //   * bind_to_any_port(host)    -> int    (the OS-picked port,
        //                                          or -1 on failure)
        // The earlier implementation always called bind_to_port and
        // assigned its bool to an int, which made `actual < 0` dead
        // and clobbered bound_port to 0/1 — breaking actual_port()
        // entirely when config.port == 0 (OS pick).
        std::uint16_t resolved_port = 0;
        if (config.port == 0) {
            int picked = server().bind_to_any_port(config.host);
            if (picked < 0) {
                running = false;
                return false;
            }
            resolved_port = static_cast<std::uint16_t>(picked);
        } else {
            if (!server().bind_to_port(config.host, config.port)) {
                running = false;
                return false;
            }
            resolved_port = config.port;
        }
        bound_port = resolved_port;

        server_thread = std::thread([this] {
            server().listen_after_bind();
        });

        heartbeat_thread = std::thread([this] {
            while (running) {
                std::this_thread::sleep_for(
                    std::chrono::seconds(config.heartbeat_sec));
                if (!running) break;
                json env = {{wire::fields::kType, wire::event_types::kPing}};
                broadcast(env.dump());
            }
        });

        return true;
    }

    void stop_server() {
        if (!running.exchange(false)) return;
        server().stop();

        // Close all SSE clients so their content providers return.
        {
            std::lock_guard<std::mutex> lk(sse_mu);
            for (auto& c : sse_clients) c->close();
            sse_clients.clear();
        }

        if (server_thread.joinable())    server_thread.join();
        if (heartbeat_thread.joinable()) heartbeat_thread.join();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// HttpAdapter — public methods
// ─────────────────────────────────────────────────────────────────────────────

HttpAdapter::HttpAdapter(HttpAdapterConfig config)
    : p_(std::make_unique<Impl>(std::move(config))) {}

HttpAdapter::~HttpAdapter() { p_->stop_server(); }

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
    std::lock_guard<std::mutex> lk(p_->registry_mu);
    auto v = std::make_unique<HttpView>(id, kind);
    HttpView& ref = *v;
    p_->views[id] = std::move(v);
    return ref;
}

HttpView* HttpAdapter::find_view(std::string_view id) noexcept {
    std::lock_guard<std::mutex> lk(p_->registry_mu);
    auto it = p_->views.find(std::string(id));
    return it == p_->views.end() ? nullptr : it->second.get();
}

void HttpAdapter::unregister_view(std::string_view id) {
    std::unique_ptr<HttpView> dead;
    {
        std::lock_guard<std::mutex> lk(p_->registry_mu);
        auto it = p_->views.find(std::string(id));
        if (it != p_->views.end()) {
            dead = std::move(it->second);
            p_->views.erase(it);
        }
    }
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

namespace {
inline HttpView& cast_view(binding::IView& v) {
    return static_cast<HttpView&>(v);
}
}

// ── Text ───────────────────────────────────────────────────────────────────

void HttpAdapter::set_text(binding::IView& v, std::string_view text) {
    auto& hv = cast_view(v);
    {
        std::lock_guard<std::mutex> lk(p_->registry_mu);
        p_->shadow[hv.id()] = std::string(text);
    }
    p_->broadcast_state(hv.id(), wire::field_kinds::kText,
                        json(std::string(text)));
}

std::string HttpAdapter::get_text(binding::IView& v) {
    auto& hv = cast_view(v);
    std::lock_guard<std::mutex> lk(p_->registry_mu);
    auto it = p_->shadow.find(hv.id());
    if (it == p_->shadow.end() || !it->second.is_string()) return "";
    return it->second.get<std::string>();
}

::aria::Subscription HttpAdapter::on_text_changed(
        binding::IView& v, std::function<void(std::string_view)> cb) {
    auto& hv = cast_view(v);
    auto id = p_->next_sub_id.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(p_->registry_mu);
        p_->on_text[hv.id()][id] = std::move(cb);
    }
    Impl* impl = p_.get();
    std::string view_id = hv.id();
    return ::aria::Subscription([impl, view_id, id]() {
        std::lock_guard<std::mutex> lk(impl->registry_mu);
        auto it = impl->on_text.find(view_id);
        if (it != impl->on_text.end()) it->second.erase(id);
    });
}

// ── Bool ───────────────────────────────────────────────────────────────────

void HttpAdapter::set_bool(binding::IView& v, bool value) {
    auto& hv = cast_view(v);
    {
        std::lock_guard<std::mutex> lk(p_->registry_mu);
        p_->shadow[hv.id()] = value;
    }
    p_->broadcast_state(hv.id(), wire::field_kinds::kBool, json(value));
}

bool HttpAdapter::get_bool(binding::IView& v) {
    auto& hv = cast_view(v);
    std::lock_guard<std::mutex> lk(p_->registry_mu);
    auto it = p_->shadow.find(hv.id());
    if (it == p_->shadow.end() || !it->second.is_boolean()) return false;
    return it->second.get<bool>();
}

::aria::Subscription HttpAdapter::on_bool_changed(
        binding::IView& v, std::function<void(bool)> cb) {
    auto& hv = cast_view(v);
    auto id = p_->next_sub_id.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(p_->registry_mu);
        p_->on_bool[hv.id()][id] = std::move(cb);
    }
    Impl* impl = p_.get();
    std::string view_id = hv.id();
    return ::aria::Subscription([impl, view_id, id]() {
        std::lock_guard<std::mutex> lk(impl->registry_mu);
        auto it = impl->on_bool.find(view_id);
        if (it != impl->on_bool.end()) it->second.erase(id);
    });
}

// ── Numeric (int / int64 / uint64 / float / double) ────────────────────────

#define ARIA_HTTP_NUMERIC_IMPL(Type, Field, KindName, JsonGet, OnMap)          \
    void HttpAdapter::set_##Field(binding::IView& v, Type value) {             \
        auto& hv = cast_view(v);                                               \
        {                                                                      \
            std::lock_guard<std::mutex> lk(p_->registry_mu);                   \
            p_->shadow[hv.id()] = value;                                       \
        }                                                                      \
        p_->broadcast_state(hv.id(), KindName, json(value));                   \
    }                                                                          \
    Type HttpAdapter::get_##Field(binding::IView& v) {                         \
        auto& hv = cast_view(v);                                               \
        std::lock_guard<std::mutex> lk(p_->registry_mu);                       \
        auto it = p_->shadow.find(hv.id());                                    \
        if (it == p_->shadow.end()) return Type{};                             \
        try { return it->second.JsonGet; }                                     \
        catch (...) { return Type{}; }                                         \
    }                                                                          \
    ::aria::Subscription HttpAdapter::on_##Field##_changed(                    \
            binding::IView& v, std::function<void(Type)> cb) {                 \
        auto& hv = cast_view(v);                                               \
        auto id = p_->next_sub_id.fetch_add(1);                                \
        {                                                                      \
            std::lock_guard<std::mutex> lk(p_->registry_mu);                   \
            p_->OnMap[hv.id()][id] = std::move(cb);                            \
        }                                                                      \
        Impl* impl = p_.get();                                                 \
        std::string view_id = hv.id();                                         \
        return ::aria::Subscription([impl, view_id, id]() {                    \
            std::lock_guard<std::mutex> lk(impl->registry_mu);                 \
            auto it = impl->OnMap.find(view_id);                               \
            if (it != impl->OnMap.end()) it->second.erase(id);                 \
        });                                                                    \
    }

ARIA_HTTP_NUMERIC_IMPL(int,           int,    wire::field_kinds::kInt,
                       template get<int>(), on_int)
ARIA_HTTP_NUMERIC_IMPL(std::int64_t,  int64,  wire::field_kinds::kInt64,
                       template get<std::int64_t>(), on_int64)
ARIA_HTTP_NUMERIC_IMPL(std::uint64_t, uint64, wire::field_kinds::kUInt64,
                       template get<std::uint64_t>(), on_uint64)
ARIA_HTTP_NUMERIC_IMPL(float,         float,  wire::field_kinds::kFloat,
                       template get<float>(), on_float)
ARIA_HTTP_NUMERIC_IMPL(double,        double, wire::field_kinds::kDouble,
                       template get<double>(), on_double)

#undef ARIA_HTTP_NUMERIC_IMPL

// ── Visibility / enabled ───────────────────────────────────────────────────

void HttpAdapter::set_visible(binding::IView& v, bool visible) {
    auto& hv = cast_view(v);
    {
        std::lock_guard<std::mutex> lk(p_->registry_mu);
        p_->shadow_visible[hv.id()] = visible;
    }
    json env = {
        {wire::fields::kType,  wire::event_types::kVisibility},
        {wire::fields::kView,  hv.id()},
        {wire::fields::kValue, visible},
    };
    p_->broadcast(env.dump());
}

void HttpAdapter::set_enabled(binding::IView& v, bool enabled) {
    auto& hv = cast_view(v);
    {
        std::lock_guard<std::mutex> lk(p_->registry_mu);
        p_->shadow_enabled[hv.id()] = enabled;
    }
    json env = {
        {wire::fields::kType,  wire::event_types::kEnabled},
        {wire::fields::kView,  hv.id()},
        {wire::fields::kValue, enabled},
    };
    p_->broadcast(env.dump());
}

// ── Click ──────────────────────────────────────────────────────────────────

::aria::Subscription HttpAdapter::on_click(
        binding::IView& v, std::function<void()> cb) {
    auto& hv = cast_view(v);
    auto id = p_->next_sub_id.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(p_->registry_mu);
        p_->on_click[hv.id()][id] = std::move(cb);
    }
    Impl* impl = p_.get();
    std::string view_id = hv.id();
    return ::aria::Subscription([impl, view_id, id]() {
        std::lock_guard<std::mutex> lk(impl->registry_mu);
        auto it = impl->on_click.find(view_id);
        if (it != impl->on_click.end()) it->second.erase(id);
    });
}

// ── Custom command channel ────────────────────────────────────────────────

void HttpAdapter::register_command(std::string_view view_id,
                                    std::string_view command_name,
                                    CommandHandler handler) {
    std::lock_guard<std::mutex> lk(p_->registry_mu);
    p_->commands[{std::string(view_id), std::string(command_name)}] =
        std::move(handler);
}

void HttpAdapter::unregister_command(std::string_view view_id,
                                      std::string_view command_name) {
    std::lock_guard<std::mutex> lk(p_->registry_mu);
    p_->commands.erase({std::string(view_id), std::string(command_name)});
}

}  // namespace aria::adapters::http

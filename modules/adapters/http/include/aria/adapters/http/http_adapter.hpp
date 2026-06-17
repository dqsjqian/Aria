#pragma once
/// @file http_adapter.hpp
/// @brief HTTP/REST/SSE implementation of IViewAdapter.
///
/// HttpAdapter is the "Web sibling" of QtAdapter / AppKitAdapter /
/// UIKitAdapter. It exposes Aria ViewModels to a remote frontend
/// (typically a browser running plain JS or Vue/React/Svelte) over
/// a small REST + SSE protocol — see `wire_protocol.hpp`.
///
/// # Design goals
///
/// 1. **Drop-in replacement for any other adapter.**
///    Same IView/IViewAdapter contract → same BindingEngine wiring.
///    A ViewModel written against IViewAdapter sees no difference
///    between Qt and HTTP. You can bind one ViewModel to BOTH adapters
///    simultaneously and the binding engine fans out updates correctly.
///
/// 2. **Zero JS dependencies on the server.**
///    The adapter ships a pure-vanilla JS SDK (~12 KB unminified) that
///    works in any modern browser without a build step. Frameworks
///    like React/Vue/Svelte can wrap the SDK trivially.
///
/// 3. **Reasonable defaults; safe-by-default.**
///    Bind to localhost; rate-limit SSE clients; no CORS by default;
///    no auth (rely on the network boundary or a reverse proxy). The
///    framework's job is to ship a usable adapter, not a production
///    web server — see `docs/security.md` for hardening guidance.
///
/// # Example
///
///   #include <aria/adapters/http/http_adapter.hpp>
///   #include <aria/binding/binding_engine.hpp>
///
///   aria::core::Property<std::string> keyword;
///   aria::adapters::http::HttpAdapter http;
///   auto& search = http.register_view("search_keyword", "text");
///   aria::binding::BindingEngine bind(http);
///   bind.bind_text(search, keyword);
///   http.start();   // non-blocking; spawns server thread
///   // Open http://localhost:9090 in a browser. State now flows in
///   // both directions.
///
/// # Threading
///
/// * `register_view`/`unregister_view`/`start`/`stop` are thread-safe.
/// * IViewAdapter setters (set_text/set_bool/...) may be called from
///   any thread; they enqueue an SSE broadcast on the server's worker
///   pool and return without blocking.
/// * IViewAdapter getters return the current shadow state under a
///   shared mutex; they are O(log N) in the number of registered views.
/// * User callbacks (registered via on_text_changed/on_click/...) are
///   invoked on HTTP server worker threads. If you need to marshal back
///   to a UI/main thread, wrap the callback with your own dispatcher.

#include "aria/abi/export.hpp"
#include "aria/binding/view_adapter.hpp"
#include "http_config.hpp"
#include "http_view.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Forward declaration so consumers don't pay for the cpp-httplib include
// unless they actually use the `native_server()` escape hatch below.
namespace httplib { class Server; }

namespace aria::adapters::http {

/// HTTP/REST/SSE implementation of IViewAdapter.
class ARIA_HTTP_API HttpAdapter final : public binding::IViewAdapter {
public:
    explicit HttpAdapter(HttpAdapterConfig config = {});
    ~HttpAdapter() override;

    HttpAdapter(const HttpAdapter&) = delete;
    HttpAdapter& operator=(const HttpAdapter&) = delete;
    HttpAdapter(HttpAdapter&&) = delete;
    HttpAdapter& operator=(HttpAdapter&&) = delete;

    // ── Lifecycle ──────────────────────────────────────────────────────

    /// Start the HTTP server on a background thread.
    /// Returns true on success; false if already running or bind failed.
    bool start();

    /// Stop the server; closes all SSE connections. Idempotent. Safe to
    /// call from the destructor.
    void stop();

    /// True iff the server thread is alive and listening.
    [[nodiscard]] bool running() const noexcept;

    /// Actual bound port — useful when config.port == 0 and the OS
    /// picked. Returns 0 when not running.
    [[nodiscard]] std::uint16_t actual_port() const noexcept;

    /// Number of currently connected SSE clients.
    [[nodiscard]] std::size_t client_count() const noexcept;

    // ── View registry ──────────────────────────────────────────────────

    /// Register a logical view with stable id and kind.
    /// kind ∈ {"text","bool","int","int64","uint64","float","double","click"}.
    /// Returns a reference to the owned HttpView (lifetime-bound to the
    /// adapter). Registering the same id twice replaces the previous view
    /// (after firing its on_destroy signal).
    HttpView& register_view(std::string id, std::string kind);

    /// Look up a previously-registered view by id, or nullptr.
    [[nodiscard]] HttpView* find_view(std::string_view id) noexcept;

    /// Unregister a view (also fires its on_destroy signal so any
    /// BindingEngine subscriptions are released cleanly).
    void unregister_view(std::string_view id);

    /// Snapshot of currently registered (id, kind) pairs. Useful for
    /// diagnostics / dynamic UI generation on the client.
    struct ViewInfo {
        std::string id;
        std::string kind;
    };
    [[nodiscard]] std::vector<ViewInfo> list_views() const;

    // ── IViewAdapter ───────────────────────────────────────────────────

    [[nodiscard]] std::string_view platform_name() const noexcept override {
        return "http";
    }

    void set_text(binding::IView& v, std::string_view text) override;
    [[nodiscard]] std::string get_text(binding::IView& v) override;
    ::aria::Subscription on_text_changed(
        binding::IView& v, std::function<void(std::string_view)> cb) override;

    void set_bool(binding::IView& v, bool value) override;
    [[nodiscard]] bool get_bool(binding::IView& v) override;
    ::aria::Subscription on_bool_changed(
        binding::IView& v, std::function<void(bool)> cb) override;

    void set_int(binding::IView& v, int value) override;
    [[nodiscard]] int get_int(binding::IView& v) override;
    ::aria::Subscription on_int_changed(
        binding::IView& v, std::function<void(int)> cb) override;

    void set_int64(binding::IView& v, std::int64_t value) override;
    [[nodiscard]] std::int64_t get_int64(binding::IView& v) override;
    ::aria::Subscription on_int64_changed(
        binding::IView& v, std::function<void(std::int64_t)> cb) override;

    void set_uint64(binding::IView& v, std::uint64_t value) override;
    [[nodiscard]] std::uint64_t get_uint64(binding::IView& v) override;
    ::aria::Subscription on_uint64_changed(
        binding::IView& v, std::function<void(std::uint64_t)> cb) override;

    void set_float(binding::IView& v, float value) override;
    [[nodiscard]] float get_float(binding::IView& v) override;
    ::aria::Subscription on_float_changed(
        binding::IView& v, std::function<void(float)> cb) override;

    void set_double(binding::IView& v, double value) override;
    [[nodiscard]] double get_double(binding::IView& v) override;
    ::aria::Subscription on_double_changed(
        binding::IView& v, std::function<void(double)> cb) override;

    void set_visible(binding::IView& v, bool visible) override;
    void set_enabled(binding::IView& v, bool enabled) override;

    ::aria::Subscription on_click(
        binding::IView& v, std::function<void()> cb) override;

    // ── Custom command channel ─────────────────────────────────────────

    /// Custom command handler signature. Receives JSON args (raw string,
    /// callee may parse), returns a JSON response string (empty for void).
    using CommandHandler =
        std::function<std::string(std::string_view args_json)>;

    /// Register a custom command. Invoked when the browser sends
    /// `POST /aria/command {"view":id,"command":name,"args":{...}}`.
    /// Multiple commands per view are allowed; (view_id, command_name)
    /// is the unique key.
    void register_command(std::string_view view_id,
                          std::string_view command_name,
                          CommandHandler handler);

    /// Unregister a previously-registered command.
    void unregister_command(std::string_view view_id,
                            std::string_view command_name);

    // ── Native server escape hatch ─────────────────────────────────────

    /// Direct access to the underlying cpp-httplib server, for consumers
    /// that want to register their own REST routes / static mounts /
    /// chunked responses on the same listening socket.
    ///
    /// **Stability**: This is an escape hatch, not a stable contract.
    /// The return type leaks the implementation detail that the adapter
    /// is built on `cpp-httplib`. If we ever swap the HTTP backend, this
    /// signature changes. Higher-level use cases should prefer
    /// `register_command()` (REST-style POST handlers) or
    /// `register_view()` + binding (state pushed via SSE).
    ///
    /// **Threading**:
    /// * Must be called *before* `start()` to register routes — once the
    ///   server thread is running, mutating the route table is racy.
    /// * Available *after* construction (the underlying server is
    ///   constructed eagerly), unlike pre-1.0 versions where you had to
    ///   start first and downcast a `void*`.
    ///
    /// **Build-time visibility**: The cpp-httplib header is exposed only
    /// over the BUILD_INTERFACE include path. If you `find_package(aria)`
    /// against an installed copy, this method will be unreachable at
    /// link time on most platforms because `httplib::Server` won't be
    /// declared in the consumer translation unit. That is intentional —
    /// installed consumers should rely on the high-level API only.
    [[nodiscard]] ::httplib::Server& native_server() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace aria::adapters::http

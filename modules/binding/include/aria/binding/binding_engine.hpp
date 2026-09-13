#pragma once

#include "aria/abi/export.hpp"
#include "aria/callback_boundary.hpp"
#include "aria/command.hpp"
#include "aria/concepts.hpp"
#include "aria/diagnostics.hpp"
#include "aria/property.hpp"
#include "aria/runtime/dispatcher.hpp"
#include "aria/subscription.hpp"
#include "aria/binding/converter.hpp"
#include "aria/binding/view_adapter.hpp"

#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aria::binding {

/// BindingEngine: connects ViewModel properties to platform views via an adapter.
///
/// ── Lifetime contract ─────────────────────────────────────────────────
///   * The typical ownership shape is that the platform widget tree
///     owns every `IView` (QWidget parent-owned, NSView superview-owned,
///     ...) and the BindingEngine is a member of the corresponding
///     ViewModel / scope. Either side may outlive the other:
///
///       - If the **engine** is destroyed first (normal scope exit),
///         all bindings are released and the views are untouched.
///       - If a **view** is destroyed first, every binding wired to
///         that view is released automatically via `IView::on_destroy`,
///         so subsequent property changes do not dereference the dead
///         view. Other views bound to the same engine keep working.
///   * Destroying the BindingEngine (or calling `clear()`) releases every
///     active binding in one shot.
///
/// ── One-way vs two-way naming ─────────────────────────────────────────
///   Controls split into two families:
///     - "input" widgets (QLineEdit, QCheckBox, QSpinBox...) support both
///       directions.  `bind_text / bind_bool / bind_int / bind_int64 /
///       bind_uint64 / bind_float / bind_double` are two-way. Use
///       `bind_*_oneway` to force VM→View only (e.g. when driving a
///       QLabel or QProgressBar).
///     - "output-only" widgets: `bind_visible` / `bind_enabled` are
///       inherently one-way because the view never writes those back into
///       business state — they reflect a state decision the VM owns.
///
/// ── Threading / binding dispatch policy ──────────────────────────────
///   Native UI toolkits are main-thread-affine: AppKit/UIKit explicitly
///   forbid touching `NS/UIView` from a background thread, and Qt requires
///   widget access on the GUI thread. Aria's reactive graph is itself
///   single-threaded. BindingEngine accepts an optional dispatcher for
///   that owning thread and a policy applied in both binding directions.
///   In particular, adapters such as HTTP may deliver input from workers;
///   marshalling keeps those callbacks from accessing the graph there.
///   Application Property writes must still respect graph affinity.
///
///     * `DispatchPolicy::Direct` (default) — every
///       callback is invoked synchronously on its originating thread.
///       Use when every Property write and adapter callback originates
///       on the graph/UI thread (the common single-threaded MVVM case).
///
///     * `DispatchPolicy::SmartMarshal` (recommended for production) —
///       VM→View setters, View→VM edits and commands are invoked
///       directly when `dispatcher.is_main_thread()` is true, otherwise
///       posted to the dispatcher. Zero overhead on the UI thread, and
///       a guaranteed thread-correct path on background threads.
///
///     * `DispatchPolicy::AlwaysPost` — every binding update is posted,
///       even from the UI thread. Useful for tests that want a
///       deterministic "property-emit happens before, view-update
///       happens later" ordering, or to coalesce a synchronous burst
///       of writes into the next event-loop iteration.
///
///   Initial synchronization during bind runs inline on the owning
///   thread. Synchronous setter echoes are suppressed before posting.
///   Binding setup, clear and view/engine destruction also belong on
///   the owning thread; the dispatcher does not make the graph or the
///   binding registry safe for concurrent access.
///
///   Posted callbacks in both directions use a per-view lifetime token:
///   if the view is destroyed between `dispatcher.post(fn)` and
///   `fn()` running, the bucket's weak handle no-ops the call so the
///   posted lambda never dereferences a dead `IView`.
///
/// MSVC C4251: BindingEngine contains template methods that inline-access
/// private STL members (shared_ptr, unordered_map). Full Pimpl would require
/// explicit template instantiation for every bind_* variant, adding
/// maintenance burden with no real ABI benefit — the class is always
/// consumed through its non-template public API, and the shared_ptr members
/// point to DLL-exported interfaces. Suppression is safe.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4251)
#endif
class ARIA_BINDING_API BindingEngine {
public:
    /// Binding dispatch policy — see the class header for semantics.
    enum class DispatchPolicy {
        Direct,        ///< call inline (default)
        SmartMarshal,  ///< inline iff dispatcher.is_main_thread()
        AlwaysPost,    ///< always post, even from the UI thread
    };

    /// Convenience constructor: no dispatcher, Direct policy.
    explicit BindingEngine(std::shared_ptr<IViewAdapter> adapter);

    /// Constructor that opts into a dispatch policy.
    /// `ui_dispatcher` may be null only when `policy == Direct`.
    BindingEngine(std::shared_ptr<IViewAdapter> adapter,
                  std::shared_ptr<runtime::IDispatcher> ui_dispatcher,
                  DispatchPolicy policy = DispatchPolicy::SmartMarshal);

    /// Retires all lifetime gates before releasing the bindings. Defined
    /// out of line so teardown code is emitted once inside the library.
    ~BindingEngine();

    BindingEngine(const BindingEngine&) = delete;
    BindingEngine& operator=(const BindingEngine&) = delete;
    BindingEngine(BindingEngine&&) = delete;
    BindingEngine& operator=(BindingEngine&&) = delete;

    [[nodiscard]] IViewAdapter& adapter() noexcept { return *adapter_; }

    [[nodiscard]] DispatchPolicy dispatch_policy() const noexcept { return policy_; }
    [[nodiscard]] bool has_dispatcher() const noexcept { return static_cast<bool>(dispatcher_); }

    // ══════════════════════════════════════════════════════════════════
    //   One-way (VM→View) bindings accept any read-only reactive source
    //
    //   Every `bind_*_oneway` / `bind_visible` / `bind_enabled` comes in
    //   two flavours:
    //
    //     * a non-template `Property<T>&` overload — the original,
    //       exported-from-the-library entry point, unchanged;
    //     * a template overload constrained on `ReadOnlyReactiveOf<S, T>`,
    //       which additionally accepts `Computed<T>`.
    //
    //   Both do exactly the same thing (they share one private
    //   implementation). The split exists so the shipped symbols keep
    //   their ABI while `Computed` becomes bindable: passing a
    //   `Property<T>` still selects the non-template overload, because a
    //   non-template beats a template when the conversion sequences tie.
    //
    //   Two-way binders stay `Property<T>&`-only on purpose. A derived
    //   value has no write-back path, so `bind_text(some_computed, view)`
    //   must remain a **compile error** rather than a silently dropped
    //   edit.
    // ══════════════════════════════════════════════════════════════════

    // ══════════════════════════════════════════════════════════════════
    //   Text
    // ══════════════════════════════════════════════════════════════════
    void bind_text_oneway(Property<std::string>& prop, IView& view);

    template<ReadOnlyReactiveOf<std::string> Src>
    void bind_text_oneway(Src& src, IView& view) {
        bind_scalar_oneway_<std::string>(src, view, &IViewAdapter::set_text);
    }

    void bind_text(Property<std::string>& prop, IView& view);

    // ══════════════════════════════════════════════════════════════════
    //   Bool (checkbox / switch)
    // ══════════════════════════════════════════════════════════════════
    void bind_bool_oneway(Property<bool>& prop, IView& view);

    template<ReadOnlyReactiveOf<bool> Src>
    void bind_bool_oneway(Src& src, IView& view) {
        bind_scalar_oneway_<bool>(src, view, &IViewAdapter::set_bool);
    }

    void bind_bool(Property<bool>& prop, IView& view);

    // ══════════════════════════════════════════════════════════════════
    //   Int (slider / spinbox / progress bar)
    // ══════════════════════════════════════════════════════════════════
    void bind_int_oneway(Property<int>& prop, IView& view);

    template<ReadOnlyReactiveOf<int> Src>
    void bind_int_oneway(Src& src, IView& view) {
        bind_scalar_oneway_<int>(src, view, &IViewAdapter::set_int);
    }

    void bind_int(Property<int>& prop, IView& view);

    // ══════════════════════════════════════════════════════════════════
    //   Int64 (timestamps, IDs, 64-bit counters)
    // ══════════════════════════════════════════════════════════════════
    void bind_int64_oneway(Property<std::int64_t>& prop, IView& view);

    template<ReadOnlyReactiveOf<std::int64_t> Src>
    void bind_int64_oneway(Src& src, IView& view) {
        bind_scalar_oneway_<std::int64_t>(src, view, &IViewAdapter::set_int64);
    }

    void bind_int64(Property<std::int64_t>& prop, IView& view);

    // ══════════════════════════════════════════════════════════════════
    //   UInt64 (raw handles, non-negative counters)
    // ══════════════════════════════════════════════════════════════════
    void bind_uint64_oneway(Property<std::uint64_t>& prop, IView& view);

    template<ReadOnlyReactiveOf<std::uint64_t> Src>
    void bind_uint64_oneway(Src& src, IView& view) {
        bind_scalar_oneway_<std::uint64_t>(src, view, &IViewAdapter::set_uint64);
    }

    void bind_uint64(Property<std::uint64_t>& prop, IView& view);

    // ══════════════════════════════════════════════════════════════════
    //   Float (UISlider, CALayer opacity)
    // ══════════════════════════════════════════════════════════════════
    void bind_float_oneway(Property<float>& prop, IView& view);

    template<ReadOnlyReactiveOf<float> Src>
    void bind_float_oneway(Src& src, IView& view) {
        bind_scalar_oneway_<float>(src, view, &IViewAdapter::set_float);
    }

    void bind_float(Property<float>& prop, IView& view);

    // ══════════════════════════════════════════════════════════════════
    //   Double (QDoubleSpinBox)
    // ══════════════════════════════════════════════════════════════════
    void bind_double_oneway(Property<double>& prop, IView& view);

    template<ReadOnlyReactiveOf<double> Src>
    void bind_double_oneway(Src& src, IView& view) {
        bind_scalar_oneway_<double>(src, view, &IViewAdapter::set_double);
    }

    void bind_double(Property<double>& prop, IView& view);

    // ══════════════════════════════════════════════════════════════════
    //   Visible / Enabled — inherently one-way
    // ══════════════════════════════════════════════════════════════════
    void bind_visible(Property<bool>& prop, IView& view);

    template<ReadOnlyReactiveOf<bool> Src>
    void bind_visible(Src& src, IView& view) {
        bind_scalar_oneway_<bool>(src, view, &IViewAdapter::set_visible);
    }

    void bind_enabled(Property<bool>& prop, IView& view);

    template<ReadOnlyReactiveOf<bool> Src>
    void bind_enabled(Src& src, IView& view) {
        bind_scalar_oneway_<bool>(src, view, &IViewAdapter::set_enabled);
    }

    // ══════════════════════════════════════════════════════════════════
    //   Converter-based bindings (non-string model types → text view)
    //
    //   Use when your ViewModel exposes e.g. `Property<int>` but the View is
    //   a QLineEdit / QLabel.  Provide a Converter<T, std::string>.
    //
    //   The one-way form accepts any `ReadOnlyReactive` source, so a
    //   `Computed<T>` can feed a converted label. The two-way form stays
    //   `Property<T>&`-only — it writes back.
    // ══════════════════════════════════════════════════════════════════
    template<ReadOnlyReactive Src>
    void bind_text_converted_oneway(Src& src,
                                    IView& view,
                                    Converter<typename Src::value_type,
                                              std::string> conv) {
        bind_projected_(src, view, std::move(conv.to_view), &IViewAdapter::set_text);
    }

    template<typename T>
    void bind_text_converted(Property<T>& prop, IView& view,
                             Converter<T, std::string> conv) {
        bind_converted_(prop, view, std::move(conv), &IViewAdapter::set_text,
                        &IViewAdapter::on_text_changed);
    }

    /// Bind a model value to an integer-valued control. The converter
    /// defines valid inputs; return nullopt to reject an unselected index.
    /// Rejections and converter exceptions preserve the model and report
    /// through "binding.converter". Dispatch and echo suppression match text.
    template<typename T>
    void bind_int_converted(Property<T>& prop, IView& view, Converter<T, int> conv) {
        bind_converted_(prop, view, std::move(conv), &IViewAdapter::set_int,
                        &IViewAdapter::on_int_changed);
    }

    // ══════════════════════════════════════════════════════════════════
    //   Projected one-way text bindings (read-only labels)
    //
    //   A read-only label rarely wants the full bidirectional `Converter`
    //   machinery of `bind_text_converted` — it only ever renders VM→View
    //   and never parses text back. These two helpers take a plain
    //   projection functor `T -> std::string` and wire the one-way path,
    //   collapsing the hand-written `prop.on_changed([lbl]{ ... })` +
    //   initial-sync boilerplate that otherwise piles up in every view.
    //
    //   They accept any read-only reactive source (`Property<T>` or
    //   `Computed<T>`) and are completely async-agnostic: the same call
    //   binds an `AsyncCommand`'s `last_error_message` / `last_result`
    //   projections, a `Computed`'s formatted output, or any other
    //   model-owned value — without this engine ever naming an
    //   `aria-async` type (see the `bind_view_lifetime` note on that
    //   deliberate API-level decoupling).
    // ══════════════════════════════════════════════════════════════════

    /// Bind a read-only text view to `src`, rendered through `project`
    /// (`T -> std::string`). One-way (VM→View) only. The initial value is
    /// synced inline on the calling (UI) thread; subsequent changes go
    /// through the configured dispatch policy and are dropped safely if
    /// the view is destroyed in flight.
    ///
    /// `src` may be a `Property<T>` or a `Computed<T>` — a formatted
    /// derived value ("¥ 12.34" off a `Computed<double>`) is the archetypal
    /// case and needs no intermediate mirror property.
    template<ReadOnlyReactive Src, typename Project>
    void bind_text_projected(Src& src, IView& view, Project project) {
        bind_projected_(src, view, std::move(project), &IViewAdapter::set_text);
    }

    /// Bind a read-only text view to a reactive `std::optional<T>` source.
    /// When the optional holds a value it is rendered through `project`
    /// (`const T& -> std::string`); when it is `std::nullopt` the view
    /// shows `empty_text` (default: empty string). One-way (VM→View) only.
    ///
    /// This is the missing piece for `AsyncCommand::last_result`
    /// (`Property<std::optional<R>>`): binding a result label used to
    /// require a hand-written `on_changed` that unwrapped the optional.
    /// A `Computed<std::optional<T>>` works identically.
    template<ReadOnlyReactiveOptional Src, typename Project>
    void bind_optional_text(Src& src,
                            IView& view,
                            Project project,
                            std::string empty_text = std::string{}) {
        using Opt = typename Src::value_type;             // std::optional<T>
        using T   = typename Opt::value_type;
        static_assert(std::is_invocable_v<Project&, const T&>,
            "bind_optional_text: `project` must be callable as "
            "project(const T&) where the source holds std::optional<T>.");
        auto render = [project = std::move(project), empty_text = std::move(empty_text)]
                      (const Opt& opt) mutable -> std::string {
            return opt ? project(*opt) : empty_text;
        };
        bind_projected_(src, view, std::move(render), &IViewAdapter::set_text);
    }

    // ═════════════════════════════════════════════════════════════════
    //   Command
    // ═════════════════════════════════════════════════════════════════
    template<typename... Args>
    void bind_command(Command<Args...>& cmd, IView& view, const Args&... args) {
        auto guard_alive = ensure_alive_token_(view);
        auto command_lifetime = cmd.lifetime_token_();
        auto adapter = adapter_;
        auto dispatcher = dispatcher_;
        const auto policy = policy_;
        auto click = adapter->on_click(view,
                [&cmd, args..., guard_alive, command_lifetime, dispatcher, policy]() {
                    dispatch_to_model_(dispatcher, policy, guard_alive, {},
                        [&cmd, args..., command_lifetime] {
                            if (!command_lifetime.expired()) cmd.execute(args...);
                        });
                });
        if (!is_alive_(guard_alive) || command_lifetime.expired()) return;
        add_view_sub_(view, std::move(click));
        // The signal carries whatever truth value the publisher chose
        // (e.g. `notify_can_execute_changed(other_args...)`). For bound
        // buttons we want the enabled state to track *these specific
        // args* — recompute via `cmd.can_execute(args...)` on every
        // notification and ignore the wire payload. This restores the
        // contract that `bind_command(cmd, view, args)` keeps the view
        // in sync with `cmd.can_execute(args...)`.
        add_view_sub_(view,
            cmd.observe_can_execute(
                [&cmd, adapter, &view, guard_alive, command_lifetime,
                 dispatcher, policy, args...](bool /*payload*/) {
                    if (!is_alive_(guard_alive) || command_lifetime.expired()) return;
                    const bool can = cmd.can_execute(args...);
                    if (!is_alive_(guard_alive) || command_lifetime.expired()) return;
                    dispatch_to_view_(adapter, dispatcher, policy, guard_alive,
                        [adapter, &view, can]() {
                            adapter->set_enabled(view, can);
                        });
                }));
        if (!is_alive_(guard_alive) || command_lifetime.expired()) return;
        const bool can = cmd.can_execute(args...);
        if (is_alive_(guard_alive) && !command_lifetime.expired()) adapter->set_enabled(view, can);
    }

    // ═════════════════════════════════════════════════════════════════
    //   View lifetime hook (async cancellation, resource teardown, ...)
    // ═════════════════════════════════════════════════════════════════
    //
    // Register a callback that fires exactly once when `view` is destroyed
    // (its `IView::on_destroy` fans out and the engine clears the view's
    // subscription bucket) OR when the engine itself is destroyed / cleared
    // — whichever comes first. The callback runs on whatever thread tears
    // the view down (the UI thread, by the IView contract).
    //
    // This is the async-agnostic primitive behind "view-destroy
    // cancellation": `BindingEngine` deliberately never names an
    // `AsyncCommand` type (it takes a plain `std::function<void()>`), so
    // instead of teaching BindingEngine about `AsyncCommand`, callers wire
    // the two together themselves — even though the `binding` module as a
    // whole does link `aria-async` for `ViewModelScope` / `Navigation` —
    //
    //     AsyncCommand<void> load{ui, [](CancellationToken t) -> Task<void>{
    //         co_await fetch(t);              // cooperative cancel point
    //     }};
    //     engine.bind_command(load.trigger(), view);   // click → execute
    //     engine.bind_view_lifetime(view, [&load]{
    //         load.cancel_all_in_flight();    // view gone → cancel request
    //     });
    //
    // Now navigating away mid-request (destroying the sub-view) fires the
    // in-flight invocation's CancellationToken, so the coroutine unwinds at
    // its next probe instead of resuming against a dead view. This closes
    // the third lifetime axis (view-destroy) alongside the existing
    // VM-scope and Navigator-entry cancellation. See ROADMAP P1-H.
    //
    // Failures report through the callback boundary; teardown still completes.
    void bind_view_lifetime(IView& view, std::function<void()> on_view_destroyed) {
        if (!on_view_destroyed) return;
        // A Subscription whose deleter runs the callback. Stored in the
        // per-view bucket so it fires on view-destroy; also pinned by the
        // engine, so engine destruction / clear() fires it too.
        (void)ensure_alive_token_(view);  // make sure the bucket+destroy wiring exists
        add_view_sub_(view, Subscription{[callback = std::move(on_view_destroyed)] {
            try { callback(); }
            catch (...) { ::aria::report_callback_failure("binding.view_lifetime", std::current_exception()); }
        }});
    }

    /// Adopt an arbitrary `Subscription` into `view`'s per-view bucket.
    ///
    /// The subscription is released when `view` is destroyed (its
    /// `IView::on_destroy` fires and the engine clears the bucket) OR when
    /// the engine itself is destroyed / cleared — whichever comes first.
    /// Exactly the same lifetime the `bind_*` calls already give their own
    /// internal subscriptions.
    ///
    /// This is the escape hatch for anything the typed `bind_*` surface
    /// does not cover yet: a hand-written `prop.on_changed(...)`, a
    /// platform signal, a `Computed::on_changed(...)`. Without it every
    /// host has to invent its own per-view subscription store (and the
    /// common workaround — a process-global `std::vector<Subscription>` —
    /// leaks by construction).
    ///
    ///     auto sub = vm.total.on_changed([lbl](double v) { ... });
    ///     engine.adopt(*lbl, std::move(sub));   // released on view-destroy
    void adopt(IView& view, Subscription s) {
        if (!s) return;
        (void)ensure_alive_token_(view);  // make sure bucket + destroy wiring exists
        add_view_sub_(view, std::move(s));
    }

    /// Drop every active binding.
    void clear() noexcept;

private:
    template<class Src>
    static reactive::detail::NodeHandle source_handle_(Src& source) noexcept {
        if constexpr (std::derived_from<Src, reactive::Node>) return reactive::detail::NodeHandle{&source};
        else return {};
    }

    template<class Src>
    static bool source_alive_(const reactive::detail::NodeHandle& handle) noexcept {
        if constexpr (std::derived_from<Src, reactive::Node>) return bool(handle);
        else return true; // Custom ReadOnlyReactive sources own their lifetime contract.
    }

    template<class Src, class Project, class Setter>
    void bind_projected_(Src& src, IView& view, Project project, Setter setter) {
        using T = typename Src::value_type;
        auto alive = ensure_alive_token_(view);
        auto source = source_handle_(src);
        auto adapter = adapter_;
        auto dispatcher = dispatcher_;
        const auto policy = policy_;
        auto projection = std::make_shared<Project>(std::move(project));
        // Copy before invoking user code; a projection can destroy its source.
        T initial = src.get();
        auto rendered = (*projection)(initial);
        if (!is_alive_(alive) || !source_alive_<Src>(source)) return;
        (adapter.get()->*setter)(view, rendered);
        if (!is_alive_(alive) || !source_alive_<Src>(source)) return;
        auto sub = src.on_changed(
            [adapter, dispatcher, policy, &view, projection, alive, setter]
            (const T& value) {
                dispatch_to_view_(adapter, dispatcher, policy, alive,
                    [adapter, &view, projection, alive, setter, value] {
                        auto rendered_value = (*projection)(value);
                        if (is_alive_(alive)) (adapter.get()->*setter)(view, rendered_value);
                    });
            });
        if (is_alive_(alive)) add_view_sub_(view, std::move(sub));
    }

    template<typename T, typename Src, typename Setter>
    void bind_scalar_oneway_(Src& src, IView& view, Setter setter) {
        bind_projected_(src, view, [](const T& value) { return value; }, setter);
    }

    template<typename T, typename U, class Setter, class Subscriber>
    void bind_converted_(Property<T>& prop, IView& view, Converter<T, U> conv,
                         Setter setter, Subscriber subscriber) {
        auto guard = std::make_shared<bool>(false);
        auto alive = ensure_alive_token_(view);
        auto handle = std::make_shared<reactive::detail::NodeHandle>(&prop);
        std::weak_ptr<reactive::detail::NodeHandle> model = handle;
        // The graph thread owns the intrusive handle. Worker callbacks only
        // copy its weak_ptr; they never link/unlink or retain a NodeHandle.
        add_view_sub_(view, Subscription{std::move(handle)});
        auto adapter = adapter_;
        auto dispatcher = dispatcher_;
        const auto policy = policy_;
        auto converter = std::make_shared<Converter<T, U>>(std::move(conv));
        T initial = prop.get();
        auto rendered = converter->to_view(initial);
        if (!is_alive_(alive) || !model_alive_(model)) return;
        {
            GuardFlag g{*guard};
            (adapter.get()->*setter)(view, rendered);
        }
        if (!is_alive_(alive) || !model_alive_(model)) return;
        auto property_sub = prop.on_changed(
            [adapter, dispatcher, policy, &view, converter,
             guard, alive, model, setter](const T& value) {
                dispatch_to_view_(adapter, dispatcher, policy, alive,
                    [adapter, &view, converter, guard, alive, model, setter, value] {
                        if (!model_alive_(model)) return;
                        GuardFlag g{*guard};
                        auto converted = converter->to_view(value);
                        if (!is_alive_(alive) || !model_alive_(model)) return;
                        (adapter.get()->*setter)(view, converted);
                    });
            });
        if (!is_alive_(alive)) return;
        add_view_sub_(view, std::move(property_sub));
        auto view_sub = (adapter.get()->*subscriber)(view,
            [&prop, converter, guard, alive, model,
             dispatcher, policy](auto native_value) {
                dispatch_to_model_(dispatcher, policy, alive, guard,
                    [&prop, converter, alive, model, value = U{native_value}] {
                        if (!model_alive_(model)) return;
                        try {
                            std::optional<T> parsed = converter->try_to_model
                                ? converter->try_to_model(value)
                                : std::optional<T>{converter->to_model(value)};
                            // Converters may synchronously clear, rebind or destroy
                            // the property/view. Neither weak token pins the target.
                            if (!is_alive_(alive) || !model_alive_(model)) return;
                            if (parsed) prop.set(std::move(*parsed));
                            else ::aria::report_callback_failure("binding.converter", nullptr,
                                "converter.try_to_model rejected input");
                        } catch (...) {
                            ::aria::report_callback_failure("binding.converter", std::current_exception());
                        }
                    });
            });
        if (is_alive_(alive)) add_view_sub_(view, std::move(view_sub));
    }

    template<typename T, typename Setter, typename Subscriber, typename ToModel>
    void bind_scalar_two_way_(Property<T>& prop, IView& view,
                              Setter setter, Subscriber subscriber, ToModel to_model) {
        // Scalars use the same lifetime/dispatch/echo path as explicit converters.
        bind_converted_(prop, view,
            Converter<T, T>{[](const T& value) { return value; },
                            [to_model](const T& value) { return to_model(value); }, {}},
            setter, subscriber);
    }

    // A view bucket owns every connection and its own destroy listener.
    // It is retired before any callback can reenter clear/bind.
    struct GuardFlag {
        bool& slot;
        bool previous;
        explicit GuardFlag(bool& s) noexcept : slot(s), previous(s) { slot = true; }
        ~GuardFlag() { slot = previous; }
        GuardFlag(const GuardFlag&)            = delete;
        GuardFlag& operator=(const GuardFlag&) = delete;
    };

    struct ViewBucket {
        bool active = true; // Accessed only on the graph thread.
        std::vector<Subscription> subscriptions;
        Subscription destroy_listener;
    };
    using AliveToken = std::weak_ptr<ViewBucket>;

    static bool is_alive_(const AliveToken& token) noexcept {
        const auto state = token.lock();
        return state && state->active;
    }
    static bool model_alive_(const std::weak_ptr<reactive::detail::NodeHandle>& token) noexcept {
        const auto handle = token.lock(); // Only called after dispatch to the graph thread.
        return handle && bool(*handle);
    }

    /// Acquire the bucket lifetime gate shared by bindings on this view.
    AliveToken ensure_alive_token_(IView& view);

    // Inbound adapter callbacks can outlive their subscription (an HTTP
    // worker may already have copied one). Capture all routing state by
    // value and check the weak token on the graph thread immediately
    // before touching Property / Command.
    template<class Fn>
    static void dispatch_to_model_(
        const std::shared_ptr<runtime::IDispatcher>& dispatcher,
        DispatchPolicy policy, AliveToken alive_token,
        std::shared_ptr<bool> guard, Fn&& fn) {
        const bool direct = !dispatcher || policy == DispatchPolicy::Direct;
        const bool on_graph_thread = direct || dispatcher->is_main_thread();

        // An AlwaysPost setter can synchronously echo while its guard is
        // active. Drop that echo now: checking only after dequeue would
        // observe a reset guard and feed formatted text back into the VM.
        // Worker callbacks must never read this graph-thread-only flag.
        if (on_graph_thread && guard && *guard) return;

        auto invoke = [alive_token, guard = std::move(guard),
                       fn = std::forward<Fn>(fn)]() mutable {
            if (!is_alive_(alive_token) || (guard && *guard)) return;
            try { fn(); }
            catch (...) { ::aria::report_callback_failure("binding.callback", std::current_exception()); }
        };
        if (direct || (policy == DispatchPolicy::SmartMarshal && on_graph_thread)) {
            invoke();
        } else {
            dispatcher->post(std::move(invoke));
        }
    }

    /// Route a VM→View callable to the configured dispatcher per the
    /// active `DispatchPolicy`. Always weak-guards on `alive_token` so
    /// a posted callback whose target view was destroyed in flight is
    /// dropped silently rather than dereferencing a dead `IView`.
    // The call sites gate payload construction. Protect construction as
    // well as publication: diagnostic allocation failure must not interrupt
    // binding delivery or teardown.
    static void trace_binding_(std::string_view platform, std::string_view operation) noexcept {
        try {
            ::aria::publish_trace_unchecked(::aria::TraceCategory::Binding,
                ::aria::trace::Binding{std::string{platform}, std::string{}, std::string{operation}});
        } catch (...) {
            ::aria::report_callback_failure("binding.trace", std::current_exception());
        }
    }

    template <class Fn>
    static void dispatch_to_view_(std::shared_ptr<IViewAdapter> adapter,
                                  std::shared_ptr<runtime::IDispatcher> dispatcher,
                                  DispatchPolicy policy, AliveToken alive, Fn&& fn) {
        auto invoke = [adapter = std::move(adapter), alive,
                       fn = std::forward<Fn>(fn)]() mutable {
            if (!is_alive_(alive)) {
                if (::aria::has_trace_sink()) trace_binding_(adapter->platform_name(), "view_destroyed_drop");
                return;
            }
            if (::aria::has_trace_sink()) trace_binding_(adapter->platform_name(), "vm_to_view");
            // Trace sinks are user callbacks and can destroy/clear the binding.
            if (is_alive_(alive)) fn();
        };
        if (!dispatcher || policy == DispatchPolicy::Direct ||
            (policy == DispatchPolicy::SmartMarshal && dispatcher->is_main_thread())) {
            invoke();
        } else {
            dispatcher->post(std::move(invoke));
        }
    }

    void add_view_sub_(IView& view, Subscription sub);

    std::shared_ptr<ViewBucket> bucket_for_(IView& view);

    std::shared_ptr<IViewAdapter>                       adapter_;
    std::shared_ptr<runtime::IDispatcher>               dispatcher_;
    DispatchPolicy                                      policy_ = DispatchPolicy::Direct;
    std::unordered_map<const IView*, std::shared_ptr<ViewBucket>> per_view_;
    bool closing_ = false;

};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

}  // namespace aria::binding

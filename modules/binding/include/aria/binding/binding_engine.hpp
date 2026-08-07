#pragma once

#include "aria/abi/export.hpp"
#include "aria/callback_boundary.hpp"
#include "aria/command.hpp"
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
/// ── Threading / VM→View dispatch policy ───────────────────────────────
///   Native UI toolkits are main-thread-affine: AppKit/UIKit explicitly
///   forbid touching `NS/UIView` from a background thread, and Qt requires
///   widget access on the GUI thread. Aria's reactive graph is itself
///   single-threaded, but `Property::set` may technically be called from
///   a worker thread by user code that has not yet hopped back to the UI
///   thread (e.g. a blocking-IO coroutine that forgot the final
///   `co_await schedule_on(ui)`). To stop that mistake from corrupting
///   the native widget tree, BindingEngine accepts an optional
///   `runtime::IDispatcher` together with a `DispatchPolicy`:
///
///     * `DispatchPolicy::Direct` (default) — every
///       VM→View setter call is invoked synchronously on whatever thread
///       the property emit happened on. Zero overhead, zero new behaviour.
///       Use when you can prove every Property write originates on the
///       UI thread (the common single-threaded MVVM case).
///
///     * `DispatchPolicy::SmartMarshal` (recommended for production) —
///       VM→View setters and command-`can_execute` updates are invoked
///       directly when `dispatcher.is_main_thread()` is true, otherwise
///       posted to the dispatcher. Zero overhead on the UI thread, and
///       a guaranteed thread-correct path on background threads.
///
///     * `DispatchPolicy::AlwaysPost` — every VM→View call is posted,
///       even from the UI thread. Useful for tests that want a
///       deterministic "property-emit happens before, view-update
///       happens later" ordering, or to coalesce a synchronous burst
///       of writes into the next event-loop iteration.
///
///   View→VM is NOT marshalled. Native callbacks already fire on the
///   UI thread by construction, so there is no race to fix on that
///   side; the View→VM path goes straight into `prop.set` /
///   `cmd.execute`, identical to the Direct path.
///
///   Posted VM→View callbacks are guarded by the per-view subscription
///   bucket: if the view is destroyed between `dispatcher.post(fn)` and
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
    /// VM→View dispatch policy — see the class header for semantics.
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

    /// Declared here, defined in binding.cpp — deliberately NOT implicit.
    ///
    /// An implicit destructor is generated inline in every consumer TU, and
    /// it expands the destructors of `per_view_` / `view_alive_` /
    /// `engine_holders_`. That makes the layout of those private members part
    /// of the effective ABI: changing one, or merely building the consumer
    /// against a different standard-library version, would break binary
    /// compatibility. The C4251 note above argues that the inline template
    /// *methods* are acceptable, but it does not cover the destructor, which
    /// every consumer emits whether or not it ever calls a template method.
    /// Pinning the definition inside the library keeps the layout private.
    ~BindingEngine();

    [[nodiscard]] IViewAdapter& adapter() noexcept { return *adapter_; }

    [[nodiscard]] DispatchPolicy dispatch_policy() const noexcept { return policy_; }
    [[nodiscard]] bool has_dispatcher() const noexcept { return static_cast<bool>(dispatcher_); }

    // ══════════════════════════════════════════════════════════════════
    //   Text
    // ══════════════════════════════════════════════════════════════════
    void bind_text_oneway(Property<std::string>& prop, IView& view);

    void bind_text(Property<std::string>& prop, IView& view);

    // ══════════════════════════════════════════════════════════════════
    //   Bool (checkbox / switch)
    // ══════════════════════════════════════════════════════════════════
    void bind_bool_oneway(Property<bool>& prop, IView& view);

    void bind_bool(Property<bool>& prop, IView& view);

    // ══════════════════════════════════════════════════════════════════
    //   Int (slider / spinbox / progress bar)
    // ══════════════════════════════════════════════════════════════════
    void bind_int_oneway(Property<int>& prop, IView& view);

    void bind_int(Property<int>& prop, IView& view);

    // ══════════════════════════════════════════════════════════════════
    //   Int64 (timestamps, IDs, 64-bit counters)
    // ══════════════════════════════════════════════════════════════════
    void bind_int64_oneway(Property<std::int64_t>& prop, IView& view);

    void bind_int64(Property<std::int64_t>& prop, IView& view);

    // ══════════════════════════════════════════════════════════════════
    //   UInt64 (raw handles, non-negative counters)
    // ══════════════════════════════════════════════════════════════════
    void bind_uint64_oneway(Property<std::uint64_t>& prop, IView& view);

    void bind_uint64(Property<std::uint64_t>& prop, IView& view);

    // ══════════════════════════════════════════════════════════════════
    //   Float (UISlider, CALayer opacity)
    // ══════════════════════════════════════════════════════════════════
    void bind_float_oneway(Property<float>& prop, IView& view);

    void bind_float(Property<float>& prop, IView& view);

    // ══════════════════════════════════════════════════════════════════
    //   Double (QDoubleSpinBox)
    // ══════════════════════════════════════════════════════════════════
    void bind_double_oneway(Property<double>& prop, IView& view);

    void bind_double(Property<double>& prop, IView& view);

    // ══════════════════════════════════════════════════════════════════
    //   Visible / Enabled — inherently one-way
    // ══════════════════════════════════════════════════════════════════
    void bind_visible(Property<bool>& prop, IView& view);

    void bind_enabled(Property<bool>& prop, IView& view);

    // ══════════════════════════════════════════════════════════════════
    //   Converter-based bindings (non-string model types → text view)
    //
    //   Use when your ViewModel exposes e.g. `Property<int>` but the View is
    //   a QLineEdit / QLabel.  Provide a Converter<T, std::string>.
    // ══════════════════════════════════════════════════════════════════
    template<typename T>
    void bind_text_converted_oneway(Property<T>& prop,
                                    IView& view,
                                    Converter<T, std::string> conv) {
        adapter_->set_text(view, conv.to_view(prop.get()));
        auto guard_alive = ensure_alive_token_(view);
        add_view_sub_(view, prop.on_changed(
            [this, adapter = adapter_, &view, to_view = std::move(conv.to_view), guard_alive]
            (const T& v) {
                this->dispatch_to_view_(guard_alive,
                    [adapter, &view, to_view, v]() {
                        adapter->set_text(view, to_view(v));
                    });
            }));
    }

    template<typename T>
    void bind_text_converted(Property<T>& prop,
                             IView& view,
                             Converter<T, std::string> conv) {
        auto guard        = std::make_shared<bool>(false);
        auto to_view      = conv.to_view;
        auto to_model     = conv.to_model;
        auto try_to_model = conv.try_to_model;
        auto guard_alive  = ensure_alive_token_(view);

        // Initial VM → View sync, guarded (RAII — restores flag even if
        // the adapter setter throws). The initial sync runs inline
        // because the constructor is documented to be called on the
        // UI thread.
        {
            GuardFlag g{*guard};
            adapter_->set_text(view, to_view(prop.get()));
        }

        add_view_sub_(view, prop.on_changed(
            [this, adapter = adapter_, &view, to_view, guard, guard_alive]
            (const T& v) {
                this->dispatch_to_view_(guard_alive,
                    [adapter, &view, to_view, guard, v]() {
                        GuardFlag g{*guard};
                        adapter->set_text(view, to_view(v));
                    });
            }));
        add_view_sub_(view, adapter_->on_text_changed(view,
            [&prop, to_model, try_to_model, guard](std::string_view sv) {
                if (*guard) return;
                std::string s(sv);
                // Preferred channel: try_to_model returns nullopt on
                // unparseable input — drop the View → Model write and
                // report once via the unified callback-boundary so the
                // host's diagnostics see it. Model retains its previous
                // value; UI keeps showing the user's bad text until the
                // adapter posts a corrected one.
                if (try_to_model) {
                    if (auto parsed = try_to_model(s)) {
                        prop.set(*parsed);
                    } else {
                        aria::report_callback_failure(
                            std::string_view{"binding.converter"},
                            nullptr,
                            std::string_view{"converter.try_to_model rejected input"});
                    }
                    return;
                }
                // Fallback channel: legacy `to_model` may throw.
                // Catch & route to the unified sink so the engine never
                // propagates user converter exceptions out of the
                // adapter callback (which is conceptually noexcept).
                try {
                    prop.set(to_model(s));
                } catch (...) {
                    aria::report_callback_failure(
                        std::string_view{"binding.converter"},
                        std::current_exception());
                }
            }));
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
    //   They operate purely on `Property<T>`, so they are completely
    //   async-agnostic: the same call binds an `AsyncCommand`'s
    //   `last_error_message` / `last_result` projections, a `Computed`'s
    //   formatted output, or any other model-owned value — without this
    //   engine ever naming an `aria-async` type (see the `bind_view_lifetime`
    //   note on that deliberate API-level decoupling).
    // ══════════════════════════════════════════════════════════════════

    /// Bind a read-only text view to `prop`, rendered through `project`
    /// (`T -> std::string`). One-way (VM→View) only. The initial value is
    /// synced inline on the calling (UI) thread; subsequent changes go
    /// through the configured dispatch policy and are dropped safely if
    /// the view is destroyed in flight.
    template<typename T, typename Project>
    void bind_text_projected(Property<T>& prop, IView& view, Project project) {
        auto guard_alive = ensure_alive_token_(view);
        adapter_->set_text(view, project(prop.get()));
        add_view_sub_(view, prop.on_changed(
            [this, adapter = adapter_, &view, project = std::move(project), guard_alive]
            (const T& v) {
                this->dispatch_to_view_(guard_alive,
                    [adapter, &view, project, v]() {
                        adapter->set_text(view, project(v));
                    });
            }));
    }

    /// Bind a read-only text view to a `Property<std::optional<T>>`.
    /// When the optional holds a value it is rendered through `project`
    /// (`const T& -> std::string`); when it is `std::nullopt` the view
    /// shows `empty_text` (default: empty string). One-way (VM→View) only.
    ///
    /// This is the missing piece for `AsyncCommand::last_result`
    /// (`Property<std::optional<R>>`): binding a result label used to
    /// require a hand-written `on_changed` that unwrapped the optional.
    template<typename T, typename Project>
    void bind_optional_text(Property<std::optional<T>>& prop,
                            IView& view,
                            Project project,
                            std::string empty_text = std::string{}) {
        auto guard_alive = ensure_alive_token_(view);
        auto render = [project = std::move(project), empty_text]
                      (const std::optional<T>& opt) -> std::string {
            return opt ? project(*opt) : empty_text;
        };
        adapter_->set_text(view, render(prop.get()));
        add_view_sub_(view, prop.on_changed(
            [this, adapter = adapter_, &view, render = std::move(render), guard_alive]
            (const std::optional<T>& opt) {
                this->dispatch_to_view_(guard_alive,
                    [adapter, &view, render, opt]() {
                        adapter->set_text(view, render(opt));
                    });
            }));
    }

    // ═════════════════════════════════════════════════════════════════
    //   Command
    // ═════════════════════════════════════════════════════════════════
    template<typename... Args>
    void bind_command(Command<Args...>& cmd, IView& view, const Args&... args) {
        auto guard_alive = ensure_alive_token_(view);
        add_view_sub_(view,
            adapter_->on_click(view, [&cmd, args...]() { cmd.execute(args...); }));
        // The signal carries whatever truth value the publisher chose
        // (e.g. `notify_can_execute_changed(other_args...)`). For bound
        // buttons we want the enabled state to track *these specific
        // args* — recompute via `cmd.can_execute(args...)` on every
        // notification and ignore the wire payload. This restores the
        // contract that `bind_command(cmd, view, args)` keeps the view
        // in sync with `cmd.can_execute(args...)`.
        add_view_sub_(view,
            cmd.observe_can_execute(
                [this, &cmd, adapter = adapter_, &view, guard_alive,
                 args...](bool /*payload*/) {
                    const bool can = cmd.can_execute(args...);
                    this->dispatch_to_view_(guard_alive,
                        [adapter, &view, can]() {
                            adapter->set_enabled(view, can);
                        });
                }));
        adapter_->set_enabled(view, cmd.can_execute(args...));
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
    // The callback must be `noexcept`-safe in spirit: it is invoked from a
    // Subscription destructor during bucket teardown. Exceptions escaping
    // it would propagate out of that destructor — keep it to cheap,
    // non-throwing teardown (cancel a token, reset a handle).
    void bind_view_lifetime(IView& view, std::function<void()> on_view_destroyed) {
        if (!on_view_destroyed) return;
        // A Subscription whose deleter runs the callback. Stored in the
        // per-view bucket so it fires on view-destroy; also pinned by the
        // engine, so engine destruction / clear() fires it too.
        (void)ensure_alive_token_(view);  // make sure the bucket+destroy wiring exists
        add_view_sub_(view, Subscription{std::move(on_view_destroyed)});
    }

    /// Drop every active binding.
    void clear() noexcept;

private:
    // -------------------------------------------------------------------
    //  VM → View (one-way) scalar binding.
    //    `Setter` is a pointer-to-member on IViewAdapter such as
    //    &IViewAdapter::set_text / set_int / set_visible ...
    // -------------------------------------------------------------------
    template<typename T, typename Setter>
    void bind_scalar_oneway_(Property<T>& prop, IView& view, Setter setter) {
        // Initial sync runs inline — BindingEngine constructors are
        // documented to be called on the UI thread.
        (adapter_.get()->*setter)(view, prop.get());
        auto guard_alive = ensure_alive_token_(view);
        add_view_sub_(view, prop.on_changed(
            [this, adapter = adapter_, &view, setter, guard_alive](const T& v) {
                this->dispatch_to_view_(guard_alive,
                    [adapter, &view, setter, v]() {
                        (adapter.get()->*setter)(view, v);
                    });
            }));
    }

    // -------------------------------------------------------------------
    //  Two-way scalar binding. `Subscriber` is a pointer-to-member that
    //  registers a view-side listener; `ToModel` converts the native
    //  callback argument (e.g. std::string_view) into the Property's T.
    //
    //  Reentrancy / feedback-loop protection
    //  -------------------------------------
    //  Many native widgets re-fire their "changed" signal when we write
    //  back to them (QLineEdit::setText → QLineEdit::textChanged on Qt).
    //  A naive two-way binding would ping-pong:
    //      VM.set(x)  →  adapter.set(view, x)
    //                 →  view emits changed
    //                 →  prop.set(x)     (no-op thanks to equality gate)
    //  The equality gate is enough for identity round-trips, but breaks
    //  as soon as there is a converter or formatter in the middle
    //  (e.g. `1 → "1.00" → 1.0` can round-trip a different value than
    //  was originally written). We guard against this explicitly with a
    //  per-binding `updating_view` flag: while we are pushing VM → View,
    //  incoming View → VM events are suppressed.
    // -------------------------------------------------------------------
    template<typename T, typename Setter, typename Subscriber, typename ToModel>
    void bind_scalar_two_way_(Property<T>& prop, IView& view,
                              Setter setter, Subscriber subscriber,
                              ToModel to_model) {
        auto guard = std::make_shared<bool>(false);
        auto guard_alive = ensure_alive_token_(view);

        // Initial sync (VM → View) under an RAII guard so any synchronous
        // "changed" echo from the setter is ignored. Using an RAII flag
        // (rather than two raw assignments around the call) guarantees
        // the guard is restored even if the adapter setter throws —
        // otherwise a stuck `true` would silently suppress every future
        // View → VM edit. The initial sync runs inline (UI-thread only).
        {
            GuardFlag g{*guard};
            (adapter_.get()->*setter)(view, prop.get());
        }

        // VM → View on every subsequent property change, also guarded
        // and routed through dispatch_to_view_ so background-thread
        // emits land safely on the UI thread when a dispatcher is set.
        add_view_sub_(view, prop.on_changed(
            [this, adapter = adapter_, &view, setter, guard, guard_alive](const T& v) {
                this->dispatch_to_view_(guard_alive,
                    [adapter, &view, setter, guard, v]() {
                        GuardFlag g{*guard};
                        (adapter.get()->*setter)(view, v);
                    });
            }));

        // View → VM, suppressed while we are the ones driving the view.
        // Native callbacks already run on the UI thread by construction,
        // so there is no marshal needed on this path.
        add_view_sub_(view, (adapter_.get()->*subscriber)(view,
            [&prop, to_model, guard](auto cb_arg) {
                if (*guard) return;
                prop.set(to_model(cb_arg));
            }));
    }

    // -------------------------------------------------------------------
    //  Per-view subscription bucket: every `bind_*` call routes through
    //  here, so that a single `view.on_destroy` callback can release
    //  ALL subscriptions tied to that view in one shot.
    //
    //  Two-level ownership:
    //    * `per_view_` holds a bucket per live view. When the view dies,
    //      `IView::on_destroy` fires, the lambda below clears the bucket
    //      (dropping every subscription from that view), and then the
    //      map entry is erased.
    //    * `engine_holders_` is a flat SubscriptionBag that pins every
    //      bucket *and* every `on_destroy` subscription for the engine's
    //      own lifetime. When the engine is destroyed, this bag drops
    //      everything — including bindings for views that are still
    //      alive, which is exactly the pre-existing contract.
    // -------------------------------------------------------------------
    // RAII scope flag used by two-way bindings to suppress View→VM
    // callbacks while a VM→View write is in flight. Restoring the flag
    // in the destructor makes the guard exception-safe (adapter setters
    // are not expected to throw, but a stuck `true` would otherwise
    // silently disable the binding for good).
    struct GuardFlag {
        bool& slot;
        explicit GuardFlag(bool& s) noexcept : slot(s) { slot = true; }
        ~GuardFlag() { slot = false; }
        GuardFlag(const GuardFlag&)            = delete;
        GuardFlag& operator=(const GuardFlag&) = delete;
    };

    using ViewBucket = std::shared_ptr<std::vector<Subscription>>;
    /// Alive-token: a weak handle to a per-view sentinel. The sentinel
    /// is held by the view's subscription bucket, so it dies precisely
    /// when `IView::on_destroy` fires and the bucket is cleared — NOT
    /// merely when the engine drops its strong ref to the bucket
    /// (the engine keeps that ref for its whole lifetime). Posted
    /// VM→View lambdas hold this and `lock()` it before touching the
    /// view; if the view was destroyed between `dispatcher.post(fn)`
    /// and `fn()` running, the lock fails and the lambda is a no-op.
    using AliveToken    = std::weak_ptr<int>;
    using AliveSentinel = std::shared_ptr<int>;

    /// Acquire (or create) the alive token for `view`. The sentinel is
    /// pushed into the view's bucket alongside its subscriptions, so it
    /// is destroyed by `bucket->clear()` inside the `on_destroy`
    /// callback. Multiple bindings on the same view share one sentinel.
    AliveToken ensure_alive_token_(IView& view);

    /// Route a VM→View callable to the configured dispatcher per the
    /// active `DispatchPolicy`. Always weak-guards on `alive_token` so
    /// a posted callback whose target view was destroyed in flight is
    /// dropped silently rather than dereferencing a dead `IView`.
    // Trace helpers — non-template to keep them out of the per-Fn template
    // body. Each call site collapses 5 lines of payload boilerplate to a
    // single helper call; the `tracing` (or `has_trace_sink()`) guard is
    // preserved at the call site so we don't pay the call-overhead when
    // diagnostics are off. Marked `noexcept` because trace publishing
    // itself is `noexcept` — see diagnostics.hpp.
    static void trace_drop_(std::string_view platform) noexcept {
        ::aria::publish_trace_unchecked(::aria::TraceCategory::Binding,
            ::aria::trace::Binding{
                std::string{platform},
                std::string{},
                "view_destroyed_drop",
            });
    }
    static void trace_emit_(std::string_view platform) noexcept {
        ::aria::publish_trace_unchecked(::aria::TraceCategory::Binding,
            ::aria::trace::Binding{
                std::string{platform},
                std::string{},
                "vm_to_view",
            });
    }

    template <class Fn>
    void dispatch_to_view_(AliveToken alive_token, Fn&& fn) {
        const bool tracing = ::aria::has_trace_sink();
        const std::string_view platform = adapter_->platform_name();

        // Liveness is established by *locking* the weak token, not by
        // querying `expired()`. `expired()` is a check-then-use: it can
        // report "alive" and the sentinel can hit zero before `fn()` runs.
        // Holding the strong reference for the duration of the call closes
        // that window and matches what both the commentary above and
        // lifecycle.md L-32 describe ("the weak handle no-ops the call").
        //
        // On the Direct / on-thread paths this is currently redundant —
        // emission and view destruction cannot interleave on one thread —
        // but it costs one refcount and removes a foot-gun for any future
        // caller that is not single-threaded. On the posted path it is load
        // bearing: the view can be destroyed between the post and the drain.

        // Direct path: no dispatcher, or policy explicitly disables
        // marshalling. Drop straight into the inline behaviour.
        if (!dispatcher_ || policy_ == DispatchPolicy::Direct) {
            auto keep_alive = alive_token.lock();
            if (!keep_alive) {
                if (tracing) trace_drop_(platform);
                return;
            }
            if (tracing) trace_emit_(platform);
            std::forward<Fn>(fn)();
            return;
        }
        if (policy_ == DispatchPolicy::SmartMarshal &&
            dispatcher_->is_main_thread()) {
            auto keep_alive = alive_token.lock();
            if (!keep_alive) {
                if (tracing) trace_drop_(platform);
                return;
            }
            if (tracing) trace_emit_(platform);
            std::forward<Fn>(fn)();
            return;
        }
        // AlwaysPost, or SmartMarshal off-thread -- marshal via dispatcher.
        std::string platform_copy{platform};
        dispatcher_->post(
            [alive_token, fn = std::forward<Fn>(fn),
             platform_copy = std::move(platform_copy)]() mutable {
                auto keep_alive = alive_token.lock();
                if (!keep_alive) {
                    if (::aria::has_trace_sink()) trace_drop_(platform_copy);
                    return;   // view died in flight
                }
                if (::aria::has_trace_sink()) trace_emit_(platform_copy);
                fn();
            });
    }

    void add_view_sub_(IView& view, Subscription sub);

    ViewBucket& bucket_for_(IView& view);

    std::shared_ptr<IViewAdapter>                       adapter_;
    std::shared_ptr<runtime::IDispatcher>               dispatcher_;
    DispatchPolicy                                      policy_ = DispatchPolicy::Direct;
    std::unordered_map<const IView*, ViewBucket>        per_view_;
    /// Per-view alive sentinel — the strong ref lives in the bucket,
    /// this map only holds another strong ref so `ensure_alive_token_`
    /// can find it cheaply on subsequent binds for the same view.
    /// Erased by the `on_destroy` callback together with `per_view_`.
    std::unordered_map<const IView*, AliveSentinel>     view_alive_;
    SubscriptionBag                                     engine_holders_;
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

}  // namespace aria::binding

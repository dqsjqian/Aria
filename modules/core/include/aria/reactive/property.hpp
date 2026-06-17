#pragma once

// ============================================================================
//  reactive/property.hpp
// ----------------------------------------------------------------------------
//  `Property<T>` is the canonical **Source node** of the reactive graph.
//
//  Design contract
//  ---------------
//  * A Property owns a value of type T and nothing else. Its identity lives
//    in the reactive graph; no hidden signal, no stored observer list of
//    its own -- those are carried by the intrusive Edge/Node primitives.
//  * Writes (`set`, `operator=`, `mutate`) bump the source's version and
//    push-color the downstream MaybeDirty. If no batch is active, the
//    Graph flushes immediately; otherwise the flush is deferred to the
//    outermost `reactive::batch([&]{ ... })` boundary.
//  * Reads (`get`, `operator T`) auto-record a dependency on the currently
//    active TrackingContext -- so Derivations and Reactions pick the
//    Property up without the user having to call `dep()` explicitly.
//  * Single-threaded. Cross-thread mutations MUST be marshalled to the
//    graph's owning thread via a Dispatcher; Debug builds assert.
//  * Equality-gated updates: `set(v)` with `v == current` is a no-op, so
//    idempotent writes cost nothing.
//
//  An earlier per-Property `.batch()` API (returning a BatchUpdate guard) is
//  deliberately omitted: a single global `reactive::batch` / `BatchScope`
//  subsumes it with a much cleaner semantics (coalescing across many
//  Properties at once).
// ============================================================================

#include "aria/reactive/graph.hpp"
#include "aria/reactive/node.hpp"

#include "aria/concepts.hpp"
#include "aria/i_property.hpp"
#include "aria/subscription.hpp"

#include <any>
#include <cassert>
#include <functional>
#include <memory>
#include <typeinfo>
#include <utility>

namespace aria::reactive {

// ---------------------------------------------------------------------------
//  Internal Reaction node used by Property::observe / on_changed / bind.
//  Declared here (not in effect.hpp) so Property can instantiate it
//  without creating a circular dependency between headers. `Effect` (the
//  public name in effect.hpp) is a thin user-facing wrapper over the same
//  primitive.
// ---------------------------------------------------------------------------
namespace detail {

class ReactionNode final : public Node {
public:
    explicit ReactionNode(std::function<void()> fn)
        : Node(NodeKind::Reaction), fn_(std::move(fn)) {}

    /// Explicit destructor: see Computed for the full rationale -- derived
    /// members are destroyed before the base, so `edges_` (holding Edge
    /// objects threaded into `sources_head_`) MUST be detached before it
    /// releases their storage; otherwise `~Node()` dereferences freed
    /// Edges via `clear_sources()`.
    ~ReactionNode() noexcept override {
        clear_sources();
    }

    /// Reactions do not produce a value; `recompute()` just runs the side
    /// effect. Returning `false` is fine: no downstream reads from us.
    bool recompute() override {
        if (fn_) fn_();
        return false;
    }

    /// Establish a one-shot dependency edge from `src` to this Reaction.
    /// Reused after the first call is harmless -- the edge is already in
    /// place and we simply refresh the observed_version.
    void observe_source(Node& src) {
        // Allocate one Edge per upstream. We own them in a small vector
        // so that destruction (our dtor -> Node::~Node -> detach) is
        // automatic and leak-free.
        edges_.push_back(std::make_unique<Edge>());
        attach_as_observer_of(src, *edges_.back());
    }

private:
    std::function<void()>           fn_;
    std::vector<std::unique_ptr<Edge>> edges_;
};

}  // namespace detail

// ---------------------------------------------------------------------------
//  Property<T>
// ---------------------------------------------------------------------------
template<PropertyValue T>
class Property : public Node, public ::aria::IProperty {
public:
    using value_type = T;

    // S-30 second tier: a one-line message that fires when somebody
    // bypasses the concept (e.g. via aliases) and lands a non-copyable
    // or non-equality-comparable type here. The concept on the template
    // header is the first line of defence; this assert exists so that
    // even when SFINAE picks up a different overload first, the eventual
    // failure points at *why*.
    static_assert(std::copyable<T>,
        "Property<T> requires T to be copyable: observers receive copies "
        "of the value on every change. If T is move-only, store it via "
        "std::shared_ptr<T> or model the state with an ObservableList<T>.");
    static_assert(EqualityComparable<T>,
        "Property<T> requires T to be equality-comparable (==/!=): writes "
        "with the same value are silently dropped. Provide an operator== "
        "for T or wrap it in a thin struct that defines one.");

    explicit Property(T initial = T{})
        : Node(NodeKind::Source), value_(std::move(initial)) {}

    // Non-copyable, non-movable: identity in the graph is tied to `this`.
    Property(const Property&)            = delete;
    Property& operator=(const Property&) = delete;
    Property(Property&&)                 = delete;
    Property& operator=(Property&&)      = delete;

    // ── Read ────────────────────────────────────────────────────────────

    /// Auto-tracked read. If a TrackingContext is active (inside a
    /// Derivation's compute or a Reaction's body), this read becomes an
    /// upstream edge of that context.
    [[nodiscard]] T get() const {
        if (auto* t = graph().current_tracker()) {
            t->record_read(const_cast<Property&>(*this));
        }
        return value_;
    }

    /// Auto-tracked read by const reference. Same auto-tracking semantics
    /// as `get()`, but avoids copying `T` on hot paths (UI bindings reading
    /// `Property<std::string>` etc.). The reference is valid until the
    /// next mutation on the graph thread; never store it across an
    /// `await` / re-entry into the graph.
    [[nodiscard]] const T& get_ref() const {
        if (auto* t = graph().current_tracker()) {
            t->record_read(const_cast<Property&>(*this));
        }
        return value_;
    }

    /// Snapshot read that does NOT register a dependency. Equivalent to
    /// `untracked([&]{ return p.get(); })` but cheaper.
    ///
    /// `noexcept` is conditional on `T`'s copy ctor — for trivially
    /// copyable types this is a free promise; for `T` that may throw on
    /// copy (e.g. `std::string` under low-memory conditions) we degrade
    /// gracefully rather than terminate.
    [[nodiscard]] T peek() const noexcept(std::is_nothrow_copy_constructible_v<T>) {
        return value_;
    }

    /// Snapshot read by const reference (never tracks, never copies).
    [[nodiscard]] const T& peek_ref() const noexcept { return value_; }

    /// Convenience: implicit conversion behaves like `.get()` so
    /// expressions read naturally (`int total = price + tax;`).
    operator T() const { return get(); }

    // ── Write ───────────────────────────────────────────────────────────

    /// Commit a new value. No-op if equal to the current one.
    /// Emits a graph-wide invalidation pulse otherwise.
    void set(const T& new_val) { set_impl_(new_val); }
    void set(T&& new_val)      { set_impl_(std::move(new_val)); }

    Property& operator=(const T& v) { set(v);            return *this; }
    Property& operator=(T&& v)      { set(std::move(v)); return *this; }

    /// Mutate-in-place: always fires a change (cannot detect no-op
    /// because the mutation is opaque). Useful for container-valued
    /// Properties where a full equality check would be expensive.
    template<std::invocable<T&> Fn>
    void mutate(Fn&& fn) {
        std::forward<Fn>(fn)(value_);
        notify_changed();
    }

    // ── Observe ─────────────────────────────────────────────────────────

    /// Run `fn(new_value)` every time the value changes. Returns a
    /// Subscription RAII handle; drop it to stop receiving callbacks.
    [[nodiscard]] ::aria::Subscription on_changed(std::function<void(const T&)> fn) {
        auto reaction = std::make_shared<detail::ReactionNode>(
            [this, fn = std::move(fn)] { fn(value_); });
        reaction->set_debug_name("Property::on_changed");
        reaction->observe_source(*this);
        return ::aria::Subscription{std::move(reaction)};
    }

    /// Fire once with the current value, then on every subsequent change.
    /// This is the idiomatic "bind a UI widget to this property" path.
    [[nodiscard]] ::aria::Subscription bind(std::function<void(const T&)> fn) {
        fn(value_);  // initial sync — outside the graph, no tracking
        return on_changed(std::move(fn));
    }

    /// Two-argument form: receive (old, new). Implemented on top of
    /// on_changed by stashing the last-seen value in a shared cell.
    [[nodiscard]] ::aria::Subscription observe(std::function<void(const T&, const T&)> fn) {
        auto last = std::make_shared<T>(value_);
        return on_changed([fn = std::move(fn), last](const T& v) {
            T old = std::move(*last);
            *last = v;
            fn(old, v);
        });
    }

    // ── Type-erased IProperty surface ───────────────────────────────────
    //
    // These methods cross the ABI boundary: callers operate on
    // `IProperty*` without knowing T. The std::any payload tunnels
    // the value through the dynamic library line. Same threading
    // contract as the typed accessors — must be invoked on the
    // graph's owning thread.

    [[nodiscard]] std::any get_any() const override {
        return std::any{get()};
    }

    [[nodiscard]] bool set_any(const std::any& value) override {
        if (auto* typed = std::any_cast<T>(&value)) {
            set(*typed);
            return true;
        }
        return false;
    }

    [[nodiscard]] ::aria::Subscription subscribe_any(
        std::function<void(const std::any&)> on_changed_any) override {
        return on_changed([cb = std::move(on_changed_any)](const T& v) {
            cb(std::any{v});
        });
    }

    [[nodiscard]] const std::type_info& type() const noexcept override {
        return typeid(T);
    }

private:
    template<class U>
    void set_impl_(U&& new_val) {
        graph().assert_on_graph_thread();
        if (value_ == new_val) return;           // equality gate -- no-op
        // Strong exception guarantee: build a temporary first so a
        // throwing T constructor cannot leave `value_` in a moved-from
        // state. The swap step is noexcept for any sane T (and
        // unconditionally so for nothrow-move-assignable types — the
        // dominant case in practice). If `T` throws on swap we are no
        // worse off than the original "value_ = ..." write.
        T tmp(std::forward<U>(new_val));
        using std::swap;
        swap(value_, tmp);
        notify_changed();                        // push-color + (maybe) flush
    }

    T value_;
};

}  // namespace aria::reactive

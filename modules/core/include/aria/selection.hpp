#pragma once

// ============================================================================
//  selection.hpp
// ----------------------------------------------------------------------------
//  Reactive selection models layered on top of `Property` and
//  `ObservableList`.
//
//    Selection<T>       — at most one selected element (nullable).
//    MultiSelection<T>  — an ordered set of selected elements
//                         (ordered by pick time).
//
//  Both store `std::shared_ptr<T>` element handles (matching
//  `ObservableList<T>`) and expose reactive state:
//
//    Selection<Item> sel;
//    sel.bind_to(list);                       // follow the source list
//    auto sub = sel.selected().on_changed([](auto& p){ highlight(p); });
//    sel.select(item);                        // -> selected() emits
//
//  `bind_to(list)` keeps the selection consistent under source mutations:
//    * an element removed from the list (Remove / Reset) drops out of the
//      selection;
//    * Move / Insert / Replace of *other* elements leaves the selection
//      intact (a selected element that is merely repositioned stays
//      selected);
//    * a Replace at the selected element's slot drops it (the logical
//      element changed identity).
//
//  Threading: same single-graph-thread contract as `Property`. The
//  `ObservableList` change stream is delivered on whatever thread mutates
//  the list; if that is not the graph thread, marshal via a Dispatcher
//  before calling `select` / `clear` (identical rule to writing any
//  Property).
//
//  Contract IDs (see docs): SE-1 single, SE-2 multi, SE-3 bind-follow,
//  SE-4 derived-list consumption (documented idiom), SE-5 memory/teardown.
// ============================================================================

#include "aria/observable_list.hpp"
#include "aria/property.hpp"
#include "aria/subscription.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

namespace aria {

// ----------------------------------------------------------------------------
//  Selection<T> — single (optional) selection.
// ----------------------------------------------------------------------------
template<typename T>
class Selection {
public:
    using value_type = T;
    using handle     = std::shared_ptr<T>;

    Selection() = default;

    Selection(const Selection&)            = delete;
    Selection& operator=(const Selection&) = delete;
    Selection(Selection&&)                 = delete;
    Selection& operator=(Selection&&)      = delete;

    /// Reactive selected handle (null when nothing is selected).
    [[nodiscard]] reactive::Property<handle>&       selected()       noexcept { return selected_; }
    [[nodiscard]] const reactive::Property<handle>& selected() const noexcept { return selected_; }

    /// Current selected element (may be null).
    [[nodiscard]] handle value() const { return selected_.get(); }
    [[nodiscard]] bool   has_value() const { return static_cast<bool>(selected_.peek()); }

    /// Select `item` (null clears). No-op if already selected (equality
    /// gate on the shared_ptr identity).
    void select(handle item) { selected_.set(std::move(item)); }

    /// Clear the selection.
    void clear() { selected_.set(nullptr); }

    /// True iff `item` is the current selection (by shared_ptr identity).
    [[nodiscard]] bool is_selected(const handle& item) const {
        return selected_.peek() == item;
    }

    /// Follow a source list: when the selected element leaves the list
    /// (Remove of that element, Replace at its slot, or Reset), the
    /// selection is cleared. Repositioning (Move/Insert/other Remove) keeps
    /// it. Returns nothing; the binding lives until this Selection dies (or
    /// `unbind()`), so a Selection must NOT outlive the list it binds to
    /// without calling `unbind()` first.
    ///
    /// Implementation note — why Replace cannot be handled by comparing
    /// `ch.item`: per the list-diff contract (D-2), a `Replace` event
    /// carries a pointer to the **new** element, not the one that was
    /// evicted. Testing `ch.item == cur.get()` therefore never matches on
    /// Replace, and the documented "a Replace at the selected element's
    /// slot drops it" behaviour silently never fired. We instead resolve
    /// the event's slot against the list itself: if the element now sitting
    /// at `ch.index` is no longer the selected handle, our selection was
    /// the one displaced.
    void bind_to(ObservableList<T>& source) {
        source_sub_ = source.observe([this, &source](const ListChange<T>& ch) {
            handle cur = selected_.peek();
            if (!cur) return;
            switch (ch.kind) {
                case ListChangeKind::Reset:
                    clear();
                    break;
                case ListChangeKind::Remove:
                    // `ch.item` IS the removed element (D-2), and the
                    // framework keeps it alive for the duration of the emit
                    // (D-3), so pointer identity is authoritative here.
                    if (ch.item == cur.get()) clear();
                    break;
                case ListChangeKind::Replace:
                    // `ch.item` is the NEW element. If it is not our
                    // selection, and our selection is no longer anywhere in
                    // the list, it was the element this Replace evicted.
                    if (ch.item != cur.get() && !source.contains(cur.get())) {
                        clear();
                    }
                    break;
                default:
                    break;  // Insert / Move / ItemChanged keep the selection
            }
        });
    }

    /// Detach from the bound source list (idempotent).
    void unbind() noexcept { source_sub_.release(); }

private:
    reactive::Property<handle> selected_{nullptr};
    Subscription               source_sub_;
};

// ----------------------------------------------------------------------------
//  MultiSelection<T> — ordered set of selected elements.
// ----------------------------------------------------------------------------
template<typename T>
class MultiSelection {
public:
    using value_type = T;
    using handle     = std::shared_ptr<T>;

    MultiSelection() = default;

    MultiSelection(const MultiSelection&)            = delete;
    MultiSelection& operator=(const MultiSelection&) = delete;
    MultiSelection(MultiSelection&&)                 = delete;
    MultiSelection& operator=(MultiSelection&&)      = delete;

    /// Reactive snapshot of selected handles, in pick order. The Property
    /// is replaced wholesale on every change so observers always get a
    /// coherent vector.
    [[nodiscard]] reactive::Property<std::vector<handle>>& selected() noexcept {
        return selected_;
    }
    [[nodiscard]] const reactive::Property<std::vector<handle>>& selected() const noexcept {
        return selected_;
    }

    [[nodiscard]] std::vector<handle> values() const { return selected_.get(); }
    [[nodiscard]] std::size_t size() const { return selected_.peek().size(); }
    [[nodiscard]] bool empty() const { return selected_.peek().empty(); }

    [[nodiscard]] bool is_selected(const handle& item) const {
        const auto& v = selected_.peek();
        return std::find(v.begin(), v.end(), item) != v.end();
    }

    /// Add `item` to the selection (appended at the end of pick order).
    /// No-op if already selected or null.
    void add(handle item) {
        if (!item) return;
        auto v = selected_.peek();
        if (std::find(v.begin(), v.end(), item) != v.end()) return;
        v.push_back(std::move(item));
        selected_.set(std::move(v));
    }

    /// Remove `item` from the selection. No-op if not selected.
    void remove(const handle& item) {
        auto v = selected_.peek();
        auto it = std::find(v.begin(), v.end(), item);
        if (it == v.end()) return;
        v.erase(it);
        selected_.set(std::move(v));
    }

    /// Toggle membership.
    void toggle(handle item) {
        if (!item) return;
        if (is_selected(item)) remove(item);
        else add(std::move(item));
    }

    void clear() {
        if (selected_.peek().empty()) return;
        selected_.set({});
    }

    /// Follow a source list: removed elements (Remove / Replace at slot)
    /// drop out of the selection; Reset clears it. Repositioning keeps the
    /// selection. See `Selection::bind_to`.
    ///
    /// As in the single-selection case, `Replace` cannot be resolved by
    /// comparing `ch.item`: that pointer is the NEW element (D-2). We drop
    /// any selected handle that is no longer a member of the list.
    void bind_to(ObservableList<T>& source) {
        source_sub_ = source.observe([this, &source](const ListChange<T>& ch) {
            if (ch.kind == ListChangeKind::Reset) { clear(); return; }
            if (ch.kind != ListChangeKind::Remove &&
                ch.kind != ListChangeKind::Replace) {
                return;
            }
            auto v = selected_.peek();
            if (v.empty()) return;

            if (ch.kind == ListChangeKind::Remove) {
                // `ch.item` IS the removed element and is kept alive for the
                // emit (D-3), so pointer identity is authoritative.
                auto it = std::find_if(v.begin(), v.end(),
                    [&](const handle& h) { return h.get() == ch.item; });
                if (it == v.end()) return;
                v.erase(it);
                selected_.set(std::move(v));
                return;
            }

            // Replace: drop every selected handle that has left the list.
            const auto new_end = std::remove_if(v.begin(), v.end(),
                [&](const handle& h) {
                    return h.get() != ch.item && !source.contains(h.get());
                });
            if (new_end == v.end()) return;  // nothing displaced
            v.erase(new_end, v.end());
            selected_.set(std::move(v));
        });
    }

    void unbind() noexcept { source_sub_.release(); }

private:
    reactive::Property<std::vector<handle>> selected_{};
    Subscription                            source_sub_;
};

}  // namespace aria

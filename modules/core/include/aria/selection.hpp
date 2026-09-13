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
#include "aria/detail/list_replay.hpp"
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
    ~Selection() { *alive_ = false; }

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

    /// Follow source membership by replaying its owning events. A removed or
    /// replaced selection clears only when no occurrence remains. Reset
    /// preserves handles present in its snapshot. The binding may outlive the
    /// source; cached handles remain valid until explicitly cleared/unbound.
    /// Bind on the source writer thread or while its writer is quiescent.
    void bind_to(ObservableList<T>& source) {
        std::weak_ptr<bool> weak_alive = alive_;
        auto rows = std::make_shared<std::vector<handle>>(source.snapshot());
        source_sub_ = source.observe([this, rows, weak_alive](const ListChange<T>& ch) {
            auto alive = weak_alive.lock();
            if (!alive || !*alive) return;
            detail::replay_list_change(*rows, ch);
            if (ch.kind != ListChangeKind::Remove && ch.kind != ListChangeKind::Replace &&
                ch.kind != ListChangeKind::Reset) return;
            auto current = selected_.peek();
            if (current && std::find(rows->begin(), rows->end(), current) == rows->end()) clear();
        });
    }

    /// Detach from the bound source list (idempotent).
    void unbind() noexcept { source_sub_.release(); }

private:
    // Signal emission snapshots may retain a disconnected callback after
    // this object is destroyed by an earlier observer on the same thread.
    std::shared_ptr<bool>       alive_ = std::make_shared<bool>(true);
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
    ~MultiSelection() { *alive_ = false; }

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

    /// Keep selected handles that survive each replayed Remove/Replace/Reset.
    /// Source destruction is safe. See Selection::bind_to for setup threading.
    void bind_to(ObservableList<T>& source) {
        std::weak_ptr<bool> weak_alive = alive_;
        auto rows = std::make_shared<std::vector<handle>>(source.snapshot());
        source_sub_ = source.observe([this, rows, weak_alive](const ListChange<T>& ch) {
            auto alive = weak_alive.lock();
            if (!alive || !*alive) return;
            detail::replay_list_change(*rows, ch);
            if (ch.kind != ListChangeKind::Remove && ch.kind != ListChangeKind::Replace &&
                ch.kind != ListChangeKind::Reset) return;
            auto selected = selected_.peek();
            const auto end = std::remove_if(selected.begin(), selected.end(), [&](const handle& item) {
                return std::find(rows->begin(), rows->end(), item) == rows->end();
            });
            if (end == selected.end()) return;
            selected.erase(end, selected.end());
            selected_.set(std::move(selected));
        });
    }

    void unbind() noexcept { source_sub_.release(); }

private:
    std::shared_ptr<bool>                    alive_ = std::make_shared<bool>(true);
    reactive::Property<std::vector<handle>> selected_{};
    Subscription                            source_sub_;
};

}  // namespace aria

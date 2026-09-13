// ============================================================================
//  aria/derived/paged_list.hpp
// ----------------------------------------------------------------------------
//  `PagedList<T>` -- a window onto a slice [page_index*page_size,
//  (page_index+1)*page_size) of an upstream `ObservableList<T>`.
//  Joins the family of derived collections and follows the
//  incremental contract of LD-2 / LD-7.
//
//  Semantics (PG-N IDs):
//
//    PG-1 (window). The derived list mirrors the source slice in
//        source order. Items outside the window are not observable
//        through PagedList.
//
//    PG-2 (live page properties). `page_index` and `page_size` are
//        public `Property`s. Changing either re-windows synchronously
//        and emits an Insert/Remove/Move diff.
//
//    PG-3 (source-driven update). Source insert / remove / replace /
//        item-changed events that fall inside the current window
//        propagate to the derived list with their derived-position
//        translated to window-local coordinates. Events outside the
//        window may slide the window content (insert before window
//        pushes a new last-item in; remove before window pulls an
//        item in from the next page).
//
//    PG-4 (page count). `page_count()` reports the number of pages
//        for the current source size + page_size, using
//        ceil-division. `is_last_page()` is a convenience.
//
//    PG-5 (lifetime). Source destruction is safe (weak source
//        observer); the cached window vector is preserved.
// ============================================================================
#pragma once

#include "aria/list_source.hpp"
#include "aria/observable_list.hpp"
#include "aria/property.hpp"
#include "aria/subscription.hpp"
#include "aria/detail/list_signal_mixin.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aria {

template<typename T, typename Source = ObservableList<T>>
    requires ListSourceOf<Source, T>
class PagedList
    : public detail::ListSignalMixin<PagedList<T, Source>, T> {
    friend detail::ListSignalMixin<PagedList<T, Source>, T>;

public:
    using value_type = T;
    using Signal     = detail::ListSignal<T>;

    /// Construct a PagedList. Both page_index (0-based) and
    /// page_size are bound to public Properties; observers can drive
    /// the window from any UI element.
    PagedList(std::shared_ptr<Source> source,
              std::size_t initial_page_size,
              std::size_t initial_page_index = 0)
        : page_size_prop_{initial_page_size == 0 ? std::size_t{1}
                                                 : initial_page_size},
          page_index_prop_{initial_page_index},
          source_(std::move(source)),
          signal_(std::make_shared<Signal>()),
          state_(std::make_shared<SharedState>())
    {
        rebuild_window_();

        std::weak_ptr<Source> weak_source{source_};
        std::weak_ptr<bool> weak_alive = alive_;
        source_sub_ = source_->observe(
            [this, weak_source, weak_alive](const ListChange<T>& ch) {
                auto alive = weak_alive.lock();
                if (!alive || !*alive) return;
                if (!weak_source.lock()) return;
                handle_source_change_(ch);
            });

        page_index_sub_ = page_index_prop_.on_changed(
            [this, weak_alive](std::size_t /*v*/) {
                auto alive = weak_alive.lock();
                if (alive && *alive) rebuild_and_emit_();
            });
        page_size_sub_  = page_size_prop_.on_changed(
            [this, weak_alive](std::size_t /*v*/) {
                auto alive = weak_alive.lock();
                if (alive && *alive) rebuild_and_emit_();
            });
    }

    ~PagedList() { *alive_ = false; }

    PagedList(const PagedList&)            = delete;
    PagedList& operator=(const PagedList&) = delete;

    // ── Read surface ──────────────────────────────────────────────────
    [[nodiscard]] std::size_t size() const {
        std::shared_lock lk(state_->m);
        return state_->window.size();
    }

    [[nodiscard]] bool empty() const { return size() == 0; }

    [[nodiscard]] std::shared_ptr<T> at(std::size_t derived_pos) const {
        std::shared_lock lk(state_->m);
        return state_->window.at(derived_pos);
    }

    [[nodiscard]] std::vector<std::shared_ptr<T>> snapshot() const {
        std::shared_lock lk(state_->m);
        return state_->window;
    }

    /// Number of pages for the current source size + page_size.
    /// 0-source -> 0 pages.
    [[nodiscard]] std::size_t page_count() const {
        const std::size_t total = source_->size();
        const std::size_t ps    = std::max<std::size_t>(1, page_size_prop_.peek());
        return total / ps + (total % ps != 0 ? 1 : 0);
    }

    [[nodiscard]] bool is_last_page() const {
        const std::size_t pc = page_count();
        return pc == 0 || page_index_prop_.peek() >= pc - 1;
    }

    // ── Public live properties (PG-2) -----------------------------------
    /// Window size in items per page. Set to drive a re-window.
    [[nodiscard]] Property<std::size_t>& page_size() noexcept {
        return page_size_prop_;
    }
    [[nodiscard]] const Property<std::size_t>& page_size() const noexcept {
        return page_size_prop_;
    }

    /// 0-based page index. Set to drive a re-window.
    [[nodiscard]] Property<std::size_t>& page_index() noexcept {
        return page_index_prop_;
    }
    [[nodiscard]] const Property<std::size_t>& page_index() const noexcept {
        return page_index_prop_;
    }

private:
    struct InputChange {
        std::optional<ListChange<T>> source_change;
        std::size_t page_size;
        std::size_t page_index;
    };

    struct SharedState {
        mutable std::shared_mutex m;
        std::vector<std::shared_ptr<T>> window;
        // Upstream may emit a multi-event diff after computing its final
        // snapshot. Replaying the source slots prevents a refill from using
        // that final snapshot for every intermediate Remove/Move.
        std::vector<std::shared_ptr<T>> source_items;
        std::size_t page_size = 1;
        std::size_t page_index = 0;
    };

    Property<std::size_t>              page_size_prop_;
    Property<std::size_t>              page_index_prop_;

    std::shared_ptr<Source> source_;
    std::shared_ptr<Signal>            signal_;
    std::shared_ptr<SharedState>       state_;
    std::shared_ptr<bool>              alive_ = std::make_shared<bool>(true);

    Subscription                       source_sub_;
    Subscription                       page_index_sub_;
    Subscription                       page_size_sub_;

    static std::vector<std::shared_ptr<T>> compute_window_(
            const std::vector<std::shared_ptr<T>>& items,
            std::size_t page_size, std::size_t page_index) {
        const auto size = std::max<std::size_t>(1, page_size);
        // Check before multiplying; both page parameters are public size_t.
        if (items.empty() || page_index > (items.size() - 1) / size) return {};
        const auto start = page_index * size;
        const auto count = std::min(size, items.size() - start);
        const auto first = items.begin() + static_cast<std::ptrdiff_t>(start);
        return {first, first + static_cast<std::ptrdiff_t>(count)};
    }

    void rebuild_window_() {
        auto items = source_->snapshot();
        const auto page_size = std::max<std::size_t>(1, page_size_prop_.peek());
        const auto page_index = page_index_prop_.peek();
        auto window = compute_window_(items, page_size, page_index);
        std::unique_lock lk(state_->m);
        state_->source_items = std::move(items);
        state_->window = std::move(window);
        state_->page_size = page_size;
        state_->page_index = page_index;
    }

    void rebuild_and_emit_() {
        auto state = state_;
        auto signal = signal_;
        apply_(*state, *signal, InputChange{
            {}, page_size_prop_.peek(), page_index_prop_.peek()});
    }

    void handle_source_change_(const ListChange<T>& change) {
        auto state = state_;
        auto signal = signal_;
        apply_(*state, *signal, InputChange{change, page_size_prop_.peek(), page_index_prop_.peek()});
    }

    static void apply_(SharedState& state, Signal& signal, InputChange event) {
        std::vector<std::shared_ptr<T>> before;
        std::vector<std::shared_ptr<T>> after;
        std::optional<ListChange<T>> direct;
        bool same_page = false;
        {
            std::unique_lock lk(state.m);
            auto& items = state.source_items;
            const auto page_size = std::max<std::size_t>(1, event.page_size);
            same_page = state.page_size == page_size && state.page_index == event.page_index;
            if (event.source_change &&
                event.source_change->kind == ListChangeKind::Insert &&
                event.source_change->index == items.size() &&
                same_page &&
                state.window.size() == page_size) {
                // A full, unchanged page cannot see a tail insertion. Still
                // replay the owned item so later page changes/refills use it.
                // Compare applied parameters: a reactive batch may already
                // have changed the Properties without re-windowing yet.
                items.push_back(event.source_change->item);
                return;
            }
            before = state.window;
            if (event.source_change) {
                const auto& ch = *event.source_change;
                const auto pos = static_cast<std::ptrdiff_t>(ch.index);
                switch (ch.kind) {
                case ListChangeKind::Insert: items.insert(items.begin() + pos, ch.item); break;
                case ListChangeKind::Remove: items.erase(items.begin() + pos); break;
                case ListChangeKind::Replace: items.at(ch.index) = ch.item; break;
                case ListChangeKind::ItemChanged: break;
                case ListChangeKind::Reset: items = *ch.snapshot; break;
                case ListChangeKind::Move: {
                    auto moved = items.at(ch.from_index);
                    items.erase(items.begin() + static_cast<std::ptrdiff_t>(ch.from_index));
                    items.insert(items.begin() + pos, std::move(moved));
                    break;
                }
                }
            }
            after = compute_window_(items, event.page_size, event.page_index);
            state.window = after;
            state.page_size = page_size;
            state.page_index = event.page_index;
            if (event.source_change) {
                const auto& ch = *event.source_change;
                if (ch.kind == ListChangeKind::Reset) {
                    direct = ListChange<T>::reset(after);
                } else if ((ch.kind == ListChangeKind::Replace ||
                            ch.kind == ListChangeKind::ItemChanged) && !after.empty()) {
                    const auto start = event.page_index * std::max<std::size_t>(1, event.page_size);
                    if (ch.index >= start && ch.index - start < after.size()) {
                        direct = ListChange<T>{ch.kind, ch.index - start, ch.item, 0};
                    }
                }
            }
        }
        if (direct && (same_page || direct->kind == ListChangeKind::Reset))
            signal.emit(*direct);
        else
            emit_diff_(signal, before, after, std::move(direct));
    }

    // Keep surviving handles in place whenever possible; repeated handles
    // retain their multiplicity. Both snapshots own all emitted payloads.
    static void emit_diff_(Signal& sig,
                           const std::vector<std::shared_ptr<T>>& before,
                           const std::vector<std::shared_ptr<T>>& after,
                           std::optional<ListChange<T>> refresh = {}) {
        std::vector<ListChange<T>> changes;
        changes.reserve(before.size() + after.size());
        std::unordered_set<const T*> in_after;
        in_after.reserve(after.size());
        for (const auto& p : after) in_after.insert(p.get());

        std::vector<std::shared_ptr<T>> work = before;
        for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(work.size()) - 1;
             i >= 0; --i) {
            const auto u = static_cast<std::size_t>(i);
            if (!in_after.count(work[u].get())) {
                changes.push_back(ListChange<T>{ListChangeKind::Remove, u,
                                       work[u], 0});
                work.erase(work.begin() + i);
            }
        }
        for (std::size_t i = 0; i < after.size(); ++i) {
            const T* want = after[i].get();
            if (i < work.size() && work[i].get() == want) continue;

            const auto pos = work.begin() + static_cast<std::ptrdiff_t>(i);
            const auto existing = std::find_if(pos, work.end(),
                [want](const auto& item) { return item.get() == want; });
            if (existing != work.end()) {
                const auto from = static_cast<std::size_t>(existing - work.begin());
                std::rotate(pos, existing, existing + 1);
                changes.push_back(ListChange<T>{ListChangeKind::Move, i, after[i], from});
                continue;
            }

            work.insert(work.begin() + static_cast<std::ptrdiff_t>(i),
                        after[i]);
            changes.push_back(ListChange<T>{ListChangeKind::Insert, i,
                                   after[i], 0});
        }

        // Membership alone does not account for repeated handles. A
        // smaller window may retain an identity but fewer occurrences.
        while (work.size() > after.size()) {
            const auto index = work.size() - 1;
            auto removed = work.back();
            work.pop_back();
            changes.push_back(ListChange<T>{ListChangeKind::Remove, index,
                                   removed, 0});
        }
        // A source content event may also apply pending page parameters.
        // Re-window first, then refresh even when the identities match. Keep
        // both in one batch so reentrant emissions cannot split their order.
        if (refresh) changes.push_back(std::move(*refresh));
        sig.emit_batch(std::move(changes));
    }
};

// ---------------------------------------------------------------------------
//  Factory helper — deduces the source type so pipelines stay readable.
//  See the note on `aria::filtered` in filtered_list.hpp.
// ---------------------------------------------------------------------------
template<typename Source, typename T = list_source_value_t<Source>>
    requires ListSourceOf<Source, T>
[[nodiscard]] std::shared_ptr<PagedList<T, Source>>
paged(std::shared_ptr<Source> source,
      std::size_t page_size,
      std::size_t page_index = 0) {
    return std::make_shared<PagedList<T, Source>>(
        std::move(source), page_size, page_index);
}

}  // namespace aria

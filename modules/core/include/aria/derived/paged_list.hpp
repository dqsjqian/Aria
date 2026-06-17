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
//        and emits a minimal Insert/Remove/Reset diff.
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

#include "aria/observable_list.hpp"
#include "aria/property.hpp"
#include "aria/subscription.hpp"
#include "aria/detail/list_signal_mixin.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aria {

template<typename T>
class PagedList
    : public detail::ListSignalMixin<PagedList<T>, T> {
    friend detail::ListSignalMixin<PagedList<T>, T>;

public:
    using value_type = T;
    using Signal     = detail::TypedSignal<ListChange<T>>;

    /// Construct a PagedList. Both page_index (0-based) and
    /// page_size are bound to public Properties; observers can drive
    /// the window from any UI element.
    PagedList(std::shared_ptr<ObservableList<T>> source,
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

        std::weak_ptr<ObservableList<T>> weak_source{source_};
        source_sub_ = source_->observe(
            [this, weak_source](const ListChange<T>& ch) {
                if (!weak_source.lock()) return;
                handle_source_change_(ch);
            });

        page_index_sub_ = page_index_prop_.on_changed(
            [this](std::size_t /*v*/) { rebuild_and_emit_(); });
        page_size_sub_  = page_size_prop_.on_changed(
            [this](std::size_t /*v*/) { rebuild_and_emit_(); });
    }

    ~PagedList() = default;

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
        return (total + ps - 1) / ps;
    }

    [[nodiscard]] bool is_last_page() const {
        const std::size_t pc = page_count();
        return pc == 0 || page_index_prop_.peek() + 1 >= pc;
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
    struct SharedState {
        mutable std::shared_mutex          m;
        std::vector<std::shared_ptr<T>>    window;
    };

    Property<std::size_t>              page_size_prop_;
    Property<std::size_t>              page_index_prop_;

    std::shared_ptr<ObservableList<T>> source_;
    std::shared_ptr<Signal>            signal_;
    std::shared_ptr<SharedState>       state_;

    Subscription                       source_sub_;
    Subscription                       page_index_sub_;
    Subscription                       page_size_sub_;

    /// Compute the desired window from current source + properties.
    [[nodiscard]] std::vector<std::shared_ptr<T>> compute_window_() const {
        const std::size_t ps    = std::max<std::size_t>(1, page_size_prop_.peek());
        const std::size_t pi    = page_index_prop_.peek();
        const std::size_t start = ps * pi;
        const std::size_t total = source_->size();
        if (start >= total) return {};
        const std::size_t end = std::min(start + ps, total);
        std::vector<std::shared_ptr<T>> out;
        out.reserve(end - start);
        for (std::size_t i = start; i < end; ++i) {
            out.push_back(source_->at(i));   // O(1) per element, no full snapshot copy
        }
        return out;
    }

    /// Initial seed -- no diff emission, just install the window.
    void rebuild_window_() {
        auto next = compute_window_();
        std::unique_lock lk(state_->m);
        state_->window = std::move(next);
    }

    /// Recompute window and emit minimal diff. Called from page
    /// property changes (PG-2). For source-driven changes, prefer
    /// the incremental `handle_source_change_` path below to keep
    /// per-event cost O(page_size) instead of O(source_size).
    void rebuild_and_emit_() {
        std::vector<std::shared_ptr<T>> old_window;
        std::vector<std::shared_ptr<T>> new_window = compute_window_();
        {
            std::unique_lock lk(state_->m);
            old_window     = state_->window;
            state_->window = new_window;
        }
        emit_diff_(*signal_, old_window, new_window);
    }

    /// Incremental source-change handler. The window covers source
    /// indices [start, end) with start = page_index * page_size and
    /// end = min(start + page_size, source.size()). Each kind of
    /// source change is mapped to AT MOST one window-local Insert /
    /// Remove / Replace / ItemChanged event in O(page_size) time --
    /// never O(source.size()). This is what keeps PG-3 honest at
    /// 10^5 elements.
    void handle_source_change_(const ListChange<T>& ch) {
        const std::size_t ps = std::max<std::size_t>(1, page_size_prop_.peek());
        const std::size_t pi = page_index_prop_.peek();
        const std::size_t start = ps * pi;
        const std::size_t end_excl = start + ps;

        if (ch.kind == ListChangeKind::Reset) {
            // Source clear -> derived clear. Capture the old window
            // (already empty after source.clear() returns; we must
            // emit Reset on the derived side regardless).
            {
                std::unique_lock lk(state_->m);
                state_->window.clear();
            }
            signal_->emit(ListChange<T>{ListChangeKind::Reset, 0, nullptr, 0});
            return;
        }

        switch (ch.kind) {
        case ListChangeKind::Insert:      handle_source_insert_(ch, start, end_excl); return;
        case ListChangeKind::Remove:      handle_source_remove_(ch, start, end_excl); return;
        case ListChangeKind::Replace:     handle_source_replace_(ch, start, end_excl); return;
        case ListChangeKind::ItemChanged: handle_source_item_changed_(ch, start, end_excl); return;
        case ListChangeKind::Move:        handle_source_move_(ch, start, end_excl); return;
        case ListChangeKind::Reset:       /* handled above */ return;
        }
    }

    void handle_source_insert_(const ListChange<T>& ch,
                               std::size_t start, std::size_t end_excl) {
        // Source insert at index ch.index. Cases:
        //   ch.index >= end_excl : after the window -> no derived event,
        //                          window unchanged (window length cap
        //                          already enforced).
        //   ch.index <  start    : before the window -> the window
        //                          slides; the new last item enters,
        //                          the previous last item slides out.
        //                          But: derived list represents indices
        //                          [start, min(start+ps, src_size)) of
        //                          the source AFTER the insert. So:
        //                          (a) the previous source[start-1]
        //                              becomes source[start] -> new
        //                              window[0]; the previous window[0]
        //                              becomes window[1]; ...; the
        //                              previous window[ps-1] is pushed
        //                              out of the window; net derived
        //                              event = Insert at 0 + Remove at ps.
        //                              We synthesize this as one Insert
        //                              + one Remove if the window was
        //                              full, else one Insert at 0.
        //   start <= ch.index < end_excl : inside the window -> Insert
        //                          at (ch.index - start). If the window
        //                          was full, the previously-last item
        //                          spills out (Remove at ps).
        std::shared_ptr<T> incoming;
        std::optional<std::size_t> emit_insert_at;
        std::optional<std::size_t> emit_remove_at;
        std::shared_ptr<T>          spilled;
        {
            std::unique_lock lk(state_->m);
            const std::size_t before = state_->window.size();
            const std::size_t cap    = end_excl - start;

            if (ch.index >= end_excl) {
                // Past window -- no event.
                return;
            }
            if (ch.index < start) {
                // Pre-window slide.
                lk.unlock();
                incoming = source_->at(start);
                lk.lock();
                state_->window.insert(state_->window.begin(), incoming);
                emit_insert_at = 0;
                if (state_->window.size() > cap) {
                    spilled = state_->window.back();
                    state_->window.pop_back();
                    emit_remove_at = state_->window.size();
                }
            } else {
                // Inside window.
                const std::size_t local = ch.index - start;
                lk.unlock();
                incoming = source_->at(ch.index);
                lk.lock();
                state_->window.insert(
                    state_->window.begin() + static_cast<std::ptrdiff_t>(local),
                    incoming);
                emit_insert_at = local;
                if (state_->window.size() > cap) {
                    spilled = state_->window.back();
                    state_->window.pop_back();
                    emit_remove_at = state_->window.size();
                }
            }
            (void)before;
        }
        if (emit_insert_at.has_value()) {
            signal_->emit(ListChange<T>{ListChangeKind::Insert, *emit_insert_at,
                                        incoming.get(), 0});
        }
        if (emit_remove_at.has_value()) {
            signal_->emit(ListChange<T>{ListChangeKind::Remove, *emit_remove_at,
                                        spilled.get(), 0});
        }
    }

    void handle_source_remove_(const ListChange<T>& ch,
                               std::size_t start, std::size_t end_excl) {
        // Source remove at ch.index. Cases:
        //   ch.index >= end_excl : past window -> no event.
        //   ch.index <  start    : pre-window -> the window slides:
        //                          previous window[0] becomes the
        //                          item at start-1 (now removed) ->
        //                          out; previous window[1] becomes
        //                          window[0]; ...; a new last item
        //                          enters from source[end_excl-1]
        //                          (if it exists). Net: Remove at 0 +
        //                          maybe Insert at ps-1.
        //   start <= ch.index < end_excl : inside window -> Remove at
        //                          (ch.index - start). If a successor
        //                          source slot exists at end_excl-1
        //                          AFTER the removal it slides into
        //                          the window (Insert at ps-1).
        std::optional<std::size_t> emit_remove_at;
        std::optional<std::size_t> emit_insert_at;
        std::shared_ptr<T>          incoming;
        std::shared_ptr<T>          removed;
        {
            std::unique_lock lk(state_->m);
            const std::size_t cap = end_excl - start;
            if (ch.index >= end_excl) return;

            if (ch.index < start) {
                if (state_->window.empty()) return;
                removed = state_->window.front();
                state_->window.erase(state_->window.begin());
                emit_remove_at = 0;
                // Pull in tail item if exists post-remove.
                lk.unlock();
                const std::size_t tail_src = end_excl - 1 - 1;
                // After source removal, source size decreased by 1.
                // The new tail-of-window source index is end_excl-1
                // measured against the post-remove source.
                const std::size_t post_size = source_->size();
                if (end_excl - 1 < post_size) {
                    incoming = source_->at(end_excl - 1);
                }
                (void)tail_src;
                lk.lock();
                if (incoming && state_->window.size() < cap) {
                    state_->window.push_back(incoming);
                    emit_insert_at = state_->window.size() - 1;
                }
            } else {
                const std::size_t local = ch.index - start;
                if (local >= state_->window.size()) return;
                removed = state_->window[local];
                state_->window.erase(state_->window.begin()
                                         + static_cast<std::ptrdiff_t>(local));
                emit_remove_at = local;
                lk.unlock();
                const std::size_t post_size = source_->size();
                if (end_excl - 1 < post_size) {
                    incoming = source_->at(end_excl - 1);
                }
                lk.lock();
                if (incoming && state_->window.size() < cap) {
                    state_->window.push_back(incoming);
                    emit_insert_at = state_->window.size() - 1;
                }
            }
        }
        if (emit_remove_at.has_value()) {
            signal_->emit(ListChange<T>{ListChangeKind::Remove, *emit_remove_at,
                                        removed.get(), 0});
        }
        if (emit_insert_at.has_value()) {
            signal_->emit(ListChange<T>{ListChangeKind::Insert, *emit_insert_at,
                                        incoming.get(), 0});
        }
    }

    void handle_source_replace_(const ListChange<T>& ch,
                                std::size_t start, std::size_t end_excl) {
        if (ch.index < start || ch.index >= end_excl) return;
        const std::size_t local = ch.index - start;
        std::shared_ptr<T> sp_ = source_->at(ch.index);
        {
            std::unique_lock lk(state_->m);
            if (local >= state_->window.size()) return;
            state_->window[local] = sp_;
        }
        signal_->emit(ListChange<T>{ListChangeKind::Replace, local,
                                    sp_.get(), 0});
    }

    void handle_source_item_changed_(const ListChange<T>& ch,
                                     std::size_t start, std::size_t end_excl) {
        if (ch.index < start || ch.index >= end_excl) return;
        const std::size_t local = ch.index - start;
        signal_->emit(ListChange<T>{ListChangeKind::ItemChanged, local,
                                    ch.item, 0});
    }

    void handle_source_move_(const ListChange<T>& /*ch*/,
                             std::size_t /*start*/, std::size_t /*end_excl*/) {
        // Conservative implementation: a Move that crosses the
        // window boundary requires re-windowing in O(page_size).
        // Fallback to rebuild_and_emit_() which uses compute_window_
        // (also O(page_size)).
        rebuild_and_emit_();
    }

    /// Emit the minimal Remove* + Insert* sequence taking `before`
    /// to `after`, identifying items by raw shared_ptr address. Same
    /// algorithm used by DistinctList's emit_diff_, kept local to
    /// avoid the cross-header dependency.
    static void emit_diff_(Signal& sig,
                           const std::vector<std::shared_ptr<T>>& before,
                           const std::vector<std::shared_ptr<T>>& after) {
        std::unordered_set<const T*> in_after;
        in_after.reserve(after.size());
        for (const auto& p : after) in_after.insert(p.get());

        std::vector<std::shared_ptr<T>> work = before;
        for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(work.size()) - 1;
             i >= 0; --i) {
            const auto u = static_cast<std::size_t>(i);
            if (!in_after.count(work[u].get())) {
                sig.emit(ListChange<T>{ListChangeKind::Remove, u,
                                       work[u].get(), 0});
                work.erase(work.begin() + i);
            }
        }
        for (std::size_t i = 0; i < after.size(); ++i) {
            const T* want = after[i].get();
            if (i < work.size() && work[i].get() == want) continue;
            work.insert(work.begin() + static_cast<std::ptrdiff_t>(i),
                        after[i]);
            sig.emit(ListChange<T>{ListChangeKind::Insert, i,
                                   after[i].get(), 0});
        }
    }
};

}  // namespace aria

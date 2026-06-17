#pragma once

// AppKitTableSource.hpp — bridge any aria list source onto NSTableView.
//
// AppKit counterpart of `qt_list_model_adapter.hpp`. Accepts any source
// satisfying `aria::ListSourceOf<L, T>` (ObservableList / FilteredList
// / SortedList / MappedList) and turns `ListChange<T>` events into:
//
//     Insert       -> [NSTableView insertRowsAtIndexes:withAnimation:]
//     Remove       -> [NSTableView removeRowsAtIndexes:withAnimation:]
//     Replace      -> [NSTableView reloadDataForRowIndexes:columnIndexes:]
//     ItemChanged  -> [NSTableView reloadDataForRowIndexes:columnIndexes:]
//     Move         -> [NSTableView moveRowAtIndex:toIndex:]
//     Reset        -> [NSTableView reloadData]
//
// Header is .mm-only (Cocoa imports). Header-only template — same
// distribution model as `qt_list_model_adapter.hpp`.
//
// Threading
// ---------
// AppKit list mutation must happen on the main thread. Same-thread
// changes apply synchronously; off-main-thread changes are queued
// onto `dispatch_get_main_queue()`. The `shared_ptr<T>` for events
// that need it (Insert / Replace / Move) is RESOLVED at emit time,
// before the dispatch hop, mirroring the Qt adapter contract — that
// way rapid back-to-back mutations never cause a queued block to
// observe a mutated source state.
//
// Lifetime
// --------
// The bridge holds a non-owning ref to the source list (caller keeps
// it alive) and a non-owning weak ref to the `NSTableView`.
// Destructor:
//   1. detaches `Subscription` (so no further events arrive),
//   2. drains the main queue once if we're not on the main thread
//      (so any already-queued blocks finish or get cancelled),
//   3. zeroes the table's dataSource/delegate iff still pointing at
//      our ObjC object.

#include "aria/list_source.hpp"
#include "aria/observable_list.hpp"
#include "aria/subscription.hpp"

#import <Cocoa/Cocoa.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

// ─── ObjC data-source / delegate ────────────────────────────────────────

@interface AriaTableDataSource : NSObject <NSTableViewDataSource, NSTableViewDelegate>
- (instancetype)initWithRowCount:(std::function<NSInteger()>)rowCountFn
                       viewForFn:(std::function<NSView*(NSTableView*,
                                                        NSTableColumn*,
                                                        NSInteger)>)viewForFn;
@end

namespace aria::adapters::appkit {

template<typename T>
class ObservableTableSource {
public:
    /// Render callback: given the row's `shared_ptr<T>`, the table
    /// view and the column being asked about, return the `NSView*`
    /// (typically an `NSTableCellView`) to display. Use
    /// `[tableView makeViewWithIdentifier:owner:]` for cell reuse.
    using ViewForRowFn = std::function<NSView*(NSTableView*,
                                               NSTableColumn*,
                                               std::shared_ptr<T>,
                                               NSInteger /*row*/)>;

    /// Construct a binding between an aria list source and an
    /// `NSTableView`. The source must outlive `*this`.
    template<class L>
        requires ::aria::ListSourceOf<L, T>
    ObservableTableSource(NSTableView* tableView,
                          L& source,
                          ViewForRowFn view_for_row)
        : state_(std::make_shared<State>()) {
        state_->table        = tableView;
        state_->view_for_row = std::move(view_for_row);
        state_->snapshot     = source.snapshot();

        // ObjC-side function pointers consult `state_` (a shared_ptr
        // captured by value into the blocks). This keeps row data
        // access alive even if the C++ wrapper is on its way out —
        // the destructor takes care of detaching cleanly first.
        std::weak_ptr<State> weak_state = state_;

        auto row_count_fn = [weak_state]() -> NSInteger {
            if (auto s = weak_state.lock()) {
                return static_cast<NSInteger>(s->snapshot.size());
            }
            return 0;
        };
        auto view_for_fn = [weak_state](NSTableView* tv,
                                        NSTableColumn* col,
                                        NSInteger row) -> NSView* {
            auto s = weak_state.lock();
            if (!s) return nil;
            if (row < 0
                || static_cast<std::size_t>(row) >= s->snapshot.size()) {
                return nil;
            }
            auto item = s->snapshot[static_cast<std::size_t>(row)];
            if (!item) return nil;
            return s->view_for_row(tv, col, item, row);
        };

        state_->ds = [[AriaTableDataSource alloc]
                          initWithRowCount:std::move(row_count_fn)
                                 viewForFn:std::move(view_for_fn)];
        tableView.dataSource = state_->ds;
        tableView.delegate   = state_->ds;
        [tableView reloadData];

        // Subscribe last — every helper below references `state_`
        // through a weak_ptr, so a dropped bridge won't crash a
        // late event delivery.
        sub_ = source.observe(
            [weak_state, &source](const ::aria::ListChange<T>& ch) {
                auto s = weak_state.lock();
                if (!s) return;
                if (s->detached.load(std::memory_order_acquire)) return;

                // Resolve the new item RIGHT NOW for events that need
                // it. For Move, the item already lives at ch.index in
                // the source post-move; for Insert / Replace the
                // source has applied the change before emitting.
                std::shared_ptr<T> resolved;
                using K = ::aria::ListChangeKind;
                if (ch.kind == K::Insert
                    || ch.kind == K::Replace
                    || ch.kind == K::Move) {
                    if (ch.index < source.size()) {
                        resolved = source.at(ch.index);
                    }
                }

                apply_on_main_(weak_state, ch, std::move(resolved));
            });
    }

    /// Detach in destruction order:
    ///   1. Drop subscription so the source stops calling us.
    ///   2. Mark `state_->detached`; any block already in-flight on
    ///      the main queue will see the flag and bail out.
    ///   3. Zero the table's dataSource/delegate iff still pointing
    ///      at our ObjC object — this MUST happen before `state_`
    ///      drops so the table doesn't briefly hold a dangling
    ///      `unsafe_unretained` data-source.
    ~ObservableTableSource() {
        sub_ = ::aria::Subscription{};
        if (state_) {
            state_->detached.store(true, std::memory_order_release);
            NSTableView* tv = state_->table;
            if (tv) {
                if (tv.dataSource == state_->ds) tv.dataSource = nil;
                if (tv.delegate   == state_->ds) tv.delegate   = nil;
            }
        }
    }

    ObservableTableSource(const ObservableTableSource&)            = delete;
    ObservableTableSource& operator=(const ObservableTableSource&) = delete;

    /// Read the bridge's current row count (mainly for tests).
    [[nodiscard]] std::size_t row_count() const noexcept {
        return state_ ? state_->snapshot.size() : 0;
    }

    /// Return the bridge's local snapshot at row `i` (or `nullptr`).
    [[nodiscard]] std::shared_ptr<T> at(std::size_t i) const {
        if (!state_) return nullptr;
        if (i >= state_->snapshot.size()) return nullptr;
        return state_->snapshot[i];
    }

private:
    struct State {
        NSTableView* __weak                 table = nil;
        AriaTableDataSource* __strong       ds    = nil;
        ViewForRowFn                        view_for_row;
        std::vector<std::shared_ptr<T>>     snapshot;
        std::atomic<bool>                   detached{false};
    };

    static void apply_on_main_(std::weak_ptr<State> weak_state,
                               ::aria::ListChange<T> ch,
                               std::shared_ptr<T> resolved) {
        auto run_now = [weak_state, ch, resolved]() {
            auto s = weak_state.lock();
            if (!s) return;
            if (s->detached.load(std::memory_order_acquire)) return;
            apply_change_(*s, ch, resolved);
        };
        if ([NSThread isMainThread]) {
            run_now();
        } else {
            __block auto blk = run_now;
            dispatch_async(dispatch_get_main_queue(), ^{ blk(); });
        }
    }

    static void apply_change_(State& s,
                              const ::aria::ListChange<T>& ch,
                              const std::shared_ptr<T>& resolved) {
        using K = ::aria::ListChangeKind;
        switch (ch.kind) {
        case K::Insert:      apply_insert_(s, ch.index, resolved);       return;
        case K::Remove:      apply_remove_(s, ch.index);                 return;
        case K::Replace:     apply_replace_(s, ch.index, resolved);      return;
        case K::ItemChanged: apply_item_changed_(s, ch.index);           return;
        case K::Move:        apply_move_(s, ch.from_index, ch.index);    return;
        case K::Reset:       apply_reset_(s);                            return;
        }
    }

    static void apply_insert_(State& s,
                              std::size_t idx,
                              const std::shared_ptr<T>& item) {
        if (idx > s.snapshot.size()) idx = s.snapshot.size();
        s.snapshot.insert(s.snapshot.begin() + static_cast<std::ptrdiff_t>(idx),
                          item);
        if (!s.table) return;
        NSIndexSet* set = [NSIndexSet indexSetWithIndex:idx];
        [s.table insertRowsAtIndexes:set
                       withAnimation:NSTableViewAnimationEffectFade];
    }

    static void apply_remove_(State& s, std::size_t idx) {
        if (idx >= s.snapshot.size()) return;
        s.snapshot.erase(s.snapshot.begin() + static_cast<std::ptrdiff_t>(idx));
        if (!s.table) return;
        NSIndexSet* set = [NSIndexSet indexSetWithIndex:idx];
        [s.table removeRowsAtIndexes:set
                       withAnimation:NSTableViewAnimationEffectFade];
    }

    static void apply_replace_(State& s,
                               std::size_t idx,
                               const std::shared_ptr<T>& item) {
        if (idx >= s.snapshot.size()) return;
        s.snapshot[idx] = item;
        if (!s.table) return;
        NSIndexSet* rowSet = [NSIndexSet indexSetWithIndex:idx];
        NSIndexSet* colSet = [NSIndexSet
            indexSetWithIndexesInRange:NSMakeRange(0, s.table.numberOfColumns)];
        [s.table reloadDataForRowIndexes:rowSet columnIndexes:colSet];
    }

    static void apply_item_changed_(State& s, std::size_t idx) {
        if (idx >= s.snapshot.size()) return;
        if (!s.table) return;
        NSIndexSet* rowSet = [NSIndexSet indexSetWithIndex:idx];
        NSIndexSet* colSet = [NSIndexSet
            indexSetWithIndexesInRange:NSMakeRange(0, s.table.numberOfColumns)];
        [s.table reloadDataForRowIndexes:rowSet columnIndexes:colSet];
    }

    static void apply_move_(State& s, std::size_t from, std::size_t to) {
        if (from == to) return;
        if (from >= s.snapshot.size() || to >= s.snapshot.size()) return;
        auto moved = s.snapshot[from];
        s.snapshot.erase(s.snapshot.begin() + static_cast<std::ptrdiff_t>(from));
        s.snapshot.insert(s.snapshot.begin() + static_cast<std::ptrdiff_t>(to),
                          std::move(moved));
        if (!s.table) return;
        [s.table moveRowAtIndex:static_cast<NSInteger>(from)
                        toIndex:static_cast<NSInteger>(to)];
    }

    static void apply_reset_(State& s) {
        // Drop our snapshot and reload the table. The source will
        // subsequently emit Inserts to repopulate (this matches
        // `ObservableList::clear`, which leaves the list empty and
        // emits a single Reset event).
        s.snapshot.clear();
        if (s.table) [s.table reloadData];
    }

    std::shared_ptr<State> state_;
    ::aria::Subscription   sub_;
};

}  // namespace aria::adapters::appkit

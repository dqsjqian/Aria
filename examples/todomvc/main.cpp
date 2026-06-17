// ============================================================================
//  Headless TodoMVC — a full, real-world ViewModel built on Aria core,
//  with no GUI toolkit. Demonstrates the derived-collection family and the
//  reactive selection model working together (ROADMAP P2-A companion /
//  audit item "large real example + derived-collection demo").
//
//  What it exercises:
//    * ObservableList<Todo>            — the source of truth.
//    * FilteredList<Todo> (x2)         — live "active" / "completed" views
//                                        that re-filter automatically when a
//                                        todo's completion state changes.
//    * Selection<Todo>                 — the currently-edited item, which
//                                        auto-clears when its item is removed.
//    * Immutable toggle via replace_at — flipping `done` replaces the item,
//                                        emitting a Replace that the derived
//                                        views translate into the minimal
//                                        Insert/Remove across the two filters.
//
//  Run as a self-checking smoke: prints a trace, returns 0 on success and
//  a non-zero code identifying the first failed assertion otherwise. Wired
//  into CTest as `todomvc_smoke`.
// ============================================================================
#include "aria/derived/filtered_list.hpp"
#include "aria/observable_list.hpp"
#include "aria/selection.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <utility>

namespace {

struct Todo {
    std::string title;
    bool        done = false;
};

// ----------------------------------------------------------------------------
//  The ViewModel. A real UI adapter (Qt/AppKit/UIKit/web) would bind its
//  table to `active()` / `completed()` and its detail pane to `editing()`.
// ----------------------------------------------------------------------------
class TodoViewModel {
public:
    TodoViewModel()
        : todos_(std::make_shared<aria::ObservableList<Todo>>()),
          active_(todos_, [](const Todo& t) { return !t.done; }),
          completed_(todos_, [](const Todo& t) { return t.done; }) {}

    aria::ObservableList<Todo>& todos()     noexcept { return *todos_; }
    aria::FilteredList<Todo>&   active()     noexcept { return active_; }
    aria::FilteredList<Todo>&   completed()  noexcept { return completed_; }
    aria::Selection<Todo>&      editing()    noexcept { return editing_; }

    void add(std::string title) {
        todos_->push_back(std::make_shared<Todo>(Todo{std::move(title), false}));
    }

    // Immutable-style toggle: replace the item with a flipped copy so the
    // derived filters re-evaluate membership (a plain in-place mutation
    // would not emit a list change).
    void toggle(std::size_t i) {
        auto cur = todos_->at(i);
        if (!cur) return;
        todos_->replace_at(i,
            std::make_shared<Todo>(Todo{cur->title, !cur->done}));
    }

    void remove(std::size_t i) { todos_->remove_at(i); }

    void clear_completed() {
        auto snap = todos_->snapshot();
        // Walk back-to-front so earlier indices stay valid as we erase.
        for (std::size_t n = snap.size(); n-- > 0;) {
            if (snap[n]->done) todos_->remove_at(n);
        }
    }

    [[nodiscard]] std::size_t total()           const { return todos_->size(); }
    [[nodiscard]] std::size_t active_count()     const { return active_.size(); }
    [[nodiscard]] std::size_t completed_count()  const { return completed_.size(); }

private:
    std::shared_ptr<aria::ObservableList<Todo>> todos_;
    aria::FilteredList<Todo>                    active_;
    aria::FilteredList<Todo>                    completed_;
    aria::Selection<Todo>                       editing_;
};

void print_state(const char* label, const TodoViewModel& vm) {
    std::printf("[%-16s] total=%zu  active=%zu  completed=%zu\n",
                label, vm.total(), vm.active_count(), vm.completed_count());
}

}  // namespace

int main() {
    TodoViewModel vm;

    // Track how often the "active" view changes — proves the derived list
    // emits incremental events, not just on add/remove.
    int active_view_changes = 0;
    auto sub = vm.active().on_any_change([&active_view_changes] {
        ++active_view_changes;
    });

    // 1. Add four todos.
    vm.add("write the RFC");
    vm.add("review the PR");
    vm.add("ship the release");
    vm.add("celebrate");
    print_state("after add", vm);
    if (vm.total() != 4 || vm.active_count() != 4 || vm.completed_count() != 0) {
        return 1;
    }

    // 2. Select the third todo for editing, then complete it.
    vm.editing().select(vm.todos().at(2));
    if (!vm.editing().has_value() || vm.editing().value()->title != "ship the release") {
        return 2;
    }

    vm.toggle(2);  // "ship the release" -> done
    print_state("after toggle", vm);
    if (vm.active_count() != 3 || vm.completed_count() != 1) {
        return 3;
    }

    // 3. Bind the selection to the source list, then remove the selected
    //    item — the selection must clear itself. (We re-select first since
    //    the toggle above replaced the item identity at index 2.)
    vm.editing().bind_to(vm.todos());
    vm.editing().select(vm.todos().at(0));
    if (vm.editing().value()->title != "write the RFC") {
        return 4;
    }
    vm.remove(0);  // removes the selected item
    print_state("after remove sel", vm);
    if (vm.editing().has_value()) {
        return 5;  // selection should have auto-cleared
    }
    if (vm.total() != 3) {
        return 6;
    }

    // 4. Complete everything still active, then clear completed.
    {
        auto snap = vm.todos().snapshot();
        for (std::size_t i = 0; i < snap.size(); ++i) {
            if (!snap[i]->done) vm.toggle(i);
        }
    }
    print_state("all completed", vm);
    if (vm.active_count() != 0 || vm.completed_count() != 3) {
        return 7;
    }

    vm.clear_completed();
    print_state("after clear", vm);
    if (vm.total() != 0) {
        return 8;
    }

    if (active_view_changes == 0) {
        return 9;  // the derived view must have fired incremental events
    }

    std::printf("todomvc: derived-collection + selection demo PASSED "
                "(active-view fired %d incremental events)\n",
                active_view_changes);
    return 0;
}

#include <doctest/doctest.h>

#include "aria/derived/filtered_list.hpp"
#include "aria/observable_list.hpp"
#include "aria/selection.hpp"

#include <memory>
#include <string>
#include <utility>

namespace {

struct Todo {
    std::string title;
    bool done = false;
};

class TodoViewModel {
public:
    TodoViewModel()
        : todos_(std::make_shared<aria::ObservableList<Todo>>()),
          active_(todos_, [](const Todo& t) { return !t.done; }),
          completed_(todos_, [](const Todo& t) { return t.done; }) {}

    aria::ObservableList<Todo>& todos() noexcept { return *todos_; }
    aria::FilteredList<Todo>& active() noexcept { return active_; }
    aria::FilteredList<Todo>& completed() noexcept { return completed_; }
    aria::Selection<Todo>& editing() noexcept { return editing_; }

    void add(std::string title) {
        todos_->push_back(std::make_shared<Todo>(Todo{std::move(title), false}));
    }

    void toggle(std::size_t i) {
        auto cur = todos_->at(i);
        if (!cur) return;
        todos_->replace_at(i,
            std::make_shared<Todo>(Todo{cur->title, !cur->done}));
    }

    void remove(std::size_t i) { todos_->remove_at(i); }

    void clear_completed() {
        auto snap = todos_->snapshot();
        for (std::size_t n = snap.size(); n-- > 0;) {
            if (snap[n]->done) todos_->remove_at(n);
        }
    }

private:
    std::shared_ptr<aria::ObservableList<Todo>> todos_;
    aria::FilteredList<Todo> active_;
    aria::FilteredList<Todo> completed_;
    aria::Selection<Todo> editing_;
};

}  // namespace

TEST_CASE("TodoMVC workflow: derived filters and selection stay coherent") {
    TodoViewModel vm;

    int active_view_changes = 0;
    auto sub = vm.active().on_any_change([&active_view_changes] {
        ++active_view_changes;
    });

    vm.add("write the RFC");
    vm.add("review the PR");
    vm.add("ship the release");
    vm.add("celebrate");
    CHECK(vm.todos().size() == 4);
    CHECK(vm.active().size() == 4);
    CHECK(vm.completed().size() == 0);

    vm.editing().select(vm.todos().at(2));
    REQUIRE(vm.editing().has_value());
    CHECK(vm.editing().value()->title == "ship the release");

    vm.toggle(2);
    CHECK(vm.active().size() == 3);
    CHECK(vm.completed().size() == 1);

    vm.editing().bind_to(vm.todos());
    vm.editing().select(vm.todos().at(0));
    REQUIRE(vm.editing().has_value());
    CHECK(vm.editing().value()->title == "write the RFC");

    vm.remove(0);
    CHECK_FALSE(vm.editing().has_value());
    CHECK(vm.todos().size() == 3);

    auto snap = vm.todos().snapshot();
    for (std::size_t i = 0; i < snap.size(); ++i) {
        if (!snap[i]->done) vm.toggle(i);
    }
    CHECK(vm.active().size() == 0);
    CHECK(vm.completed().size() == 3);

    vm.clear_completed();
    CHECK(vm.todos().size() == 0);
    CHECK(active_view_changes > 0);
}

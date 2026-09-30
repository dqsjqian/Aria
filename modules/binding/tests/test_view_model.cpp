#include <doctest/doctest.h>

#include "aria/binding/view_model.hpp"
#include <stdexcept>

using namespace aria;
using namespace aria::binding;

namespace {
class HomeVM : public ViewModel {
public:
    int activate_count = 0;
    int deactivate_count = 0;

    void on_activate() override { ++activate_count; }
    void on_deactivate() override { ++deactivate_count; }
};
}  // namespace

TEST_CASE("ViewModel: child activation can append another child") {
    struct AppendingVM : ViewModel {
        std::function<void()> activate_hook;
        void on_activate() override { if (activate_hook) activate_hook(); }
    };
    auto parent = std::make_shared<ViewModel>();
    auto first = std::make_shared<AppendingVM>();
    auto last = std::make_shared<HomeVM>();
    parent->add_child(first);
    parent->add_child(last);
    first->activate_hook = [&] {
        for (int i = 0; i < 100; ++i) parent->add_child(std::make_shared<ViewModel>());
    };
    parent->activate();
    CHECK(last->is_active().get());
}

TEST_CASE("ViewModel: activate/deactivate lifecycle") {
    auto vm = std::make_shared<HomeVM>();
    CHECK_FALSE(vm->is_active().get());

    vm->activate();
    CHECK(vm->is_active().get());
    CHECK(vm->activate_count == 1);

    vm->activate();  // idempotent
    CHECK(vm->activate_count == 1);

    vm->deactivate();
    CHECK_FALSE(vm->is_active().get());
    CHECK(vm->deactivate_count == 1);
}

TEST_CASE("ViewModel: child VMs propagate activate/deactivate") {
    auto parent = std::make_shared<HomeVM>();
    auto child = std::make_shared<HomeVM>();
    parent->add_child(child);

    parent->activate();
    CHECK(child->activate_count == 1);

    parent->deactivate();
    CHECK(child->deactivate_count == 1);
}

// Doctest macro internals introduce the flagged branches/type traits.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("ViewModel: indirect child cycles are rejected without retaining owners") {
    auto first = std::make_shared<HomeVM>();
    auto second = std::make_shared<HomeVM>();
    auto third = std::make_shared<HomeVM>();
    std::weak_ptr<ViewModel> weak_first = first;
    std::weak_ptr<ViewModel> weak_second = second;
    std::weak_ptr<ViewModel> weak_third = third;
    first->add_child(second);
    second->add_child(third);
    // Doctest macro internals introduce the flagged branches/type traits.
    // NOLINTNEXTLINE(modernize-type-traits)
    CHECK_THROWS_AS(third->add_child(first), std::invalid_argument);
    // Doctest macro internals introduce the flagged branches/type traits.
    // NOLINTNEXTLINE(modernize-type-traits)
    CHECK_THROWS_AS(second->add_child(first), std::invalid_argument);
    first->activate();
    CHECK(third->activate_count == 1);
    first->deactivate();
    CHECK(third->deactivate_count == 1);
    first.reset();
    second.reset();
    third.reset();
    CHECK(weak_first.expired());
    CHECK(weak_second.expired());
    CHECK(weak_third.expired());
}

TEST_CASE("ViewModel: shared descendants remain valid in an acyclic graph") {
    auto root = std::make_shared<ViewModel>();
    auto left = std::make_shared<ViewModel>();
    auto right = std::make_shared<ViewModel>();
    auto shared = std::make_shared<HomeVM>();
    left->add_child(shared);
    right->add_child(shared);
    root->add_child(left);
    CHECK_NOTHROW(root->add_child(right));
    root->activate();
    CHECK(shared->activate_count == 1);
}

// Doctest macro internals introduce the flagged branches/type traits.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("ViewModel: lifecycle hooks may reenter the same transition") {
    struct ReentrantVM : ViewModel {
        int activated = 0;
        int deactivated = 0;
        bool fail = true;
        void on_activate() override {
            ++activated;
            activate();
            if (fail) { throw std::runtime_error("activation failed"); }
        }
        void on_deactivate() override {
            ++deactivated;
            deactivate();
        }
    };
    auto vm = std::make_shared<ReentrantVM>();
    // Doctest macro internals introduce the flagged branches/type traits.
    // NOLINTNEXTLINE(modernize-type-traits)
    CHECK_THROWS_AS(vm->activate(), std::runtime_error);
    CHECK_FALSE(vm->is_active().get());
    vm->fail = false;
    CHECK_NOTHROW(vm->activate());
    CHECK(vm->is_active().get());
    CHECK(vm->activated == 2);
    CHECK_NOTHROW(vm->deactivate());
    CHECK_FALSE(vm->is_active().get());
    CHECK(vm->deactivated == 1);
}

TEST_CASE("ViewModel: active observers can deactivate after transition commits") {
    auto vm = std::make_shared<HomeVM>();
    auto subscription = vm->is_active().on_changed([&](bool active) {
        if (active) { vm->deactivate(); }
    });
    vm->activate();
    CHECK_FALSE(vm->is_active().get());
    CHECK(vm->activate_count == 1);
    CHECK(vm->deactivate_count == 1);
}

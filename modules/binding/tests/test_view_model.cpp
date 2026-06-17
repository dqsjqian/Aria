#include <doctest/doctest.h>

#include "aria/binding/view_model.hpp"

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

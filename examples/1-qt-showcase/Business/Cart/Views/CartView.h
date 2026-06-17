#pragma once
#include "aria/binding/binding_engine.hpp"
#include <QWidget>

namespace showcase::cart {

class CartVm;

QWidget* build_view(CartVm& vm, aria::binding::BindingEngine& be);

}  // namespace showcase::cart

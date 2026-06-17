#pragma once
#include "aria/binding/binding_engine.hpp"
#include <QWidget>

namespace showcase::theme {

class ThemeVm;

QWidget* build_view(ThemeVm& vm, aria::binding::BindingEngine& be);

}  // namespace showcase::theme

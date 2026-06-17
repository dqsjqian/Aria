#pragma once
#include "aria/binding/binding_engine.hpp"
#include <QWidget>

namespace showcase::tipcalc {

class TipCalcVm;

QWidget* build_view(TipCalcVm& vm, aria::binding::BindingEngine& be);

}  // namespace showcase::tipcalc

#pragma once
#include "aria/binding/binding_engine.hpp"
#include <QWidget>

namespace showcase::unitconvert {

class UnitConvertVm;

QWidget* build_view(UnitConvertVm& vm, aria::binding::BindingEngine& be);

}  // namespace showcase::unitconvert

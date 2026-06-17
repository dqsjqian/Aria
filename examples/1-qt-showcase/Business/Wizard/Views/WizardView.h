#pragma once
#include "aria/binding/binding_engine.hpp"
#include <QWidget>

namespace showcase::wizard {

class WizardVm;

QWidget* build_view(WizardVm& vm, aria::binding::BindingEngine& be);

}  // namespace showcase::wizard

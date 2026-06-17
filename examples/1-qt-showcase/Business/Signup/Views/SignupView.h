#pragma once
#include "aria/binding/binding_engine.hpp"
#include <QWidget>

namespace showcase::signup {

class SignupVm;

QWidget* build_view(SignupVm& vm, aria::binding::BindingEngine& be);

}  // namespace showcase::signup

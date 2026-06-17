#pragma once
#include "aria/binding/binding_engine.hpp"
#include <QWidget>

namespace showcase::login {

class LoginVm;

QWidget* build_view(LoginVm& vm, aria::binding::BindingEngine& be);

}  // namespace showcase::login

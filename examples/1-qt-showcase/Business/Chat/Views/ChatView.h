#pragma once
#include "aria/binding/binding_engine.hpp"
#include <QWidget>

namespace showcase::chat {

class ChatVm;

QWidget* build_view(ChatVm& vm, aria::binding::BindingEngine& be);

}  // namespace showcase::chat

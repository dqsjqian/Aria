#pragma once
#include "aria/binding/binding_engine.hpp"
#include <QWidget>

namespace showcase::search {

class SearchVm;

QWidget* build_view(SearchVm& vm, aria::binding::BindingEngine& be);

}  // namespace showcase::search

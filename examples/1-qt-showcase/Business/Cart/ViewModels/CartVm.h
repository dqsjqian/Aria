#pragma once
//
// CartVm — Tab 3: shopping cart
//
// Scenario
//   Add / remove / change quantity → live aggregation of subtotal /
//   tax / total plus a "N items" badge.
//

#include "aria/aria.hpp"
#include "aria/command.hpp"
#include "aria/observable_list.hpp"
#include "aria/binding/view_model.hpp"

#include "Business/Cart/Models/CartItem.h"

#include <string>

namespace showcase::cart {

class CartVm : public aria::binding::ViewModel {
public:
    static constexpr double kTaxRate = 0.08;

    CartVm();

    aria::ObservableList<CartItem> items;

    aria::Property<double> subtotal{0.0};
    aria::Property<double> tax{0.0};
    aria::Property<double> total{0.0};
    aria::Property<int>    itemCount{0};

    aria::Property<std::string> draftName{"Apple"};
    aria::Property<double>      draftPrice{3.5};

    aria::Command<> addItem;

    void on_activate() override;
    void on_deactivate() override;

private:
    void recompute_();
};

}  // namespace showcase::cart

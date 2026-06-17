#pragma once
//
// CartItem — a shopping-cart row.
// Implements the on_changed(...) protocol expected by ObservableList:
// when qty changes the list fires an ItemChanged event.
//

#include "aria/aria.hpp"
#include "aria/subscription.hpp"

#include <functional>
#include <string>

namespace showcase::cart {

class CartItem {
public:
    CartItem(std::string name, double price, int qty = 1);

    [[nodiscard]] const std::string&   name()      const noexcept { return name_; }
    [[nodiscard]] double               price()     const noexcept { return price_; }
    [[nodiscard]] aria::Property<int>& qty()       noexcept       { return qty_; }
    [[nodiscard]] int                  qty_value() const          { return qty_.get(); }
    [[nodiscard]] double               subtotal()  const          { return price_ * qty_.get(); }

    /// Per-item subscription protocol expected by ObservableList.
    [[nodiscard]] aria::Subscription
    on_changed(std::function<void(const CartItem&)> fn);

private:
    std::string         name_;
    double              price_;
    aria::Property<int> qty_;
};

}  // namespace showcase::cart

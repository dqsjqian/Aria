#include "CartItem.h"

#include <utility>

namespace showcase::cart {

CartItem::CartItem(std::string name, double price, int qty)
    : name_(std::move(name)), price_(price), qty_(qty) {}

aria::Subscription
CartItem::on_changed(std::function<void(const CartItem&)> fn) {
    return qty_.on_changed([this, fn = std::move(fn)](int) { fn(*this); });
}

}  // namespace showcase::cart

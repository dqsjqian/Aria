#include "CartVm.h"

#include <memory>

namespace showcase::cart {

CartVm::CartVm()
    : addItem(
          [this] {
              const auto name = draftName.get();
              if (name.empty() || draftPrice.get() <= 0) return;
              items.push_back(std::make_shared<CartItem>(
                  name, draftPrice.get(), 1));
          },
          [this] {
              return !draftName.get().empty() && draftPrice.get() > 0;
          })
{}

void CartVm::on_activate() {
    // Any list mutation (Insert / Remove / Replace / ItemChanged)
    // re-aggregates the totals.
    bag() += items.on_any_change([this] { recompute_(); });

    // Changes to the draft fields automatically refresh
    // addItem.can_execute and the UI button enables / disables itself.
    // No manual Effect + notify_can_execute_changed is required:
    // Command<>'s internal Effect already turns reads of draftName /
    // draftPrice into dependency edges.

    recompute_();
}

void CartVm::on_deactivate() {
    bag().clear();
}

void CartVm::recompute_() {
    double s = 0.0;
    int    n = 0;
    for (auto& item : items.snapshot()) {
        s += item->subtotal();
        n += item->qty_value();
    }
    aria::batch([this, s, n] {
        subtotal  = s;
        tax       = s * kTaxRate;
        total     = s + s * kTaxRate;
        itemCount = n;
    });
}

}  // namespace showcase::cart

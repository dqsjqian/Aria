#include "TipCalcVm.h"

#include <cmath>

namespace showcase::tipcalc {

TipCalcVm::TipCalcVm()
    : bill(238.0),
      tipPercent(15),
      people(2),
      tipAmount([this] { return bill.get() * tipPercent.get() / 100.0; }),
      total    ([this] { return bill.get() + tipAmount.get(); }),
      perPerson([this] {
          const int n = people.get();
          return n > 0 ? total.get() / n : 0.0;
      }),
      roundUp(
          [this] {
              aria::batch([this] {
                  bill       = std::ceil(bill.get());
                  tipPercent = ((tipPercent.get() + 4) / 5) * 5;
              });
          },
          [this] { return canRoundUp_(); })
{}

void TipCalcVm::on_activate() {
    // Nothing to wire up: `roundUp`'s predicate reads `bill` and
    // `tipPercent` reactively, and `Command<>`'s built-in Effect
    // re-evaluates / re-emits `can_execute` automatically when those
    // Properties change. The bound button stays in sync with no manual
    // notify_can_execute_changed() call needed.
}

void TipCalcVm::on_deactivate() { bag().clear(); }

bool TipCalcVm::canRoundUp_() const {
    const double b = bill.get();
    return (b != std::floor(b)) || (tipPercent.get() % 5 != 0);
}

}  // namespace showcase::tipcalc

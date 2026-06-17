#pragma once
//
// TipCalcVm — Tab 1: tip calculator
//
// Scenario
//   bill / tip % / number of people → live computation of tip /
//   total / per-person amount.
//   The "Round up" button rounds the bill to the nearest integer and
//   snaps the tip to a 5% boundary — two Property writes wrapped in
//   reactive::batch so downstream flushes once.
//
// Framework features
//   - binding::ViewModel    activate/deactivate lifecycle, bag_
//   - Property / Computed / Command / reactive::batch / Effect
//
// Lifecycle
//   Command<>::CanExecute is now automatically tracked by an internal
//   Effect that follows whichever Properties the predicate reads, so
//   on_activate / on_deactivate no longer need to install a manual
//   Effect to trigger notify_can_execute_changed — the typical case
//   leaves them empty.
//

#include "aria/aria.hpp"
#include "aria/command.hpp"
#include "aria/binding/view_model.hpp"

namespace showcase::tipcalc {

class TipCalcVm : public aria::binding::ViewModel {
public:
    TipCalcVm();

    aria::Property<double> bill;
    aria::Property<int>    tipPercent;
    aria::Property<int>    people;

    aria::Computed<double> tipAmount;
    aria::Computed<double> total;
    aria::Computed<double> perPerson;

    aria::Command<> roundUp;

    void on_activate() override;
    void on_deactivate() override;

private:
    bool canRoundUp_() const;
};

}  // namespace showcase::tipcalc

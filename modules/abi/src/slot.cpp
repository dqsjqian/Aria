#include "aria/abi/slot.hpp"

namespace aria::abi {

SlotErased::~SlotErased() noexcept {
    if (destroyer_ && state_) destroyer_(state_);
}

SlotErased::SlotErased(SlotErased&& o) noexcept
    : invoker_(o.invoker_), destroyer_(o.destroyer_), state_(o.state_) {
    o.invoker_ = nullptr;
    o.destroyer_ = nullptr;
    o.state_ = nullptr;
}

SlotErased& SlotErased::operator=(SlotErased&& o) noexcept {
    if (this != &o) {
        if (destroyer_ && state_) destroyer_(state_);
        invoker_ = o.invoker_;
        destroyer_ = o.destroyer_;
        state_ = o.state_;
        o.invoker_ = nullptr;
        o.destroyer_ = nullptr;
        o.state_ = nullptr;
    }
    return *this;
}

}  // namespace aria::abi

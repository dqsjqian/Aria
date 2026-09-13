#include "aria/abi/slot.hpp"

namespace aria::abi {

SlotErased::~SlotErased() noexcept {
    auto* destroyer = std::exchange(destroyer_, nullptr);
    auto* state = std::exchange(state_, nullptr);
    invoker_ = nullptr;
    if (destroyer && state) destroyer(state);
}

SlotErased::SlotErased(SlotErased&& o) noexcept
    : invoker_(o.invoker_), destroyer_(o.destroyer_), state_(o.state_) {
    o.invoker_ = nullptr;
    o.destroyer_ = nullptr;
    o.state_ = nullptr;
}

SlotErased& SlotErased::operator=(SlotErased&& o) noexcept {
    if (this != &o) {
        // Publish the replacement before releasing old user-owned state.
        // A destructor that re-enters this slot sees a complete value.
        SlotErased removed{std::move(*this)};
        invoker_ = std::exchange(o.invoker_, nullptr);
        destroyer_ = std::exchange(o.destroyer_, nullptr);
        state_ = std::exchange(o.state_, nullptr);
    }
    return *this;
}

}  // namespace aria::abi

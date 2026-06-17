#pragma once

#include "export.hpp"
#include <cstddef>
#include <cstdint>
#include <utility>

namespace aria::abi {

/// Opaque slot identifier returned when a callback is registered with a Signal.
/// Trivially copyable, ABI stable.
struct SlotId {
    std::uint64_t value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    constexpr bool operator==(SlotId rhs) const noexcept { return value == rhs.value; }
    constexpr bool operator!=(SlotId rhs) const noexcept { return value != rhs.value; }
};

/// Type-erased callback wrapper. Stores a callable that takes a single
/// `void*` "args" pointer (the caller is responsible for casting).
///
/// Why void* instead of std::function?
///   - Zero allocation for trivially-copyable callables (SBO-friendly).
///   - ABI stable (no template).
///   - Templates in `core/` wrap this with strong typing.
///
/// NOTE: SlotErased is move-only. `state_` is an opaque owning pointer;
/// moves transfer ownership by copying the pointer, so no "move" function
/// pointer is required (unlike some small-buffer-optimized designs).
///
/// The dtor / move ops are defined out-of-line in slot.cpp so the
/// (trivial) destruction of `state_` is not inlined into every TU,
/// which keeps code size small and improves compile times.
class ARIA_ABI_API SlotErased {
public:
    using Invoker = void (*)(void* state, void* args) noexcept;
    using Destroyer = void (*)(void* state) noexcept;

    SlotErased() noexcept = default;

    SlotErased(Invoker inv, Destroyer dtor, void* state) noexcept
        : invoker_(inv), destroyer_(dtor), state_(state) {}

    ~SlotErased() noexcept;

    SlotErased(SlotErased&& o) noexcept;
    SlotErased& operator=(SlotErased&& o) noexcept;

    SlotErased(const SlotErased&) = delete;
    SlotErased& operator=(const SlotErased&) = delete;

    void invoke(void* args) const noexcept {
        if (ARIA_LIKELY(invoker_ && state_)) invoker_(state_, args);
    }

    [[nodiscard]] bool empty() const noexcept { return invoker_ == nullptr; }

private:
    Invoker invoker_ = nullptr;
    Destroyer destroyer_ = nullptr;
    void* state_ = nullptr;
};

}  // namespace aria::abi

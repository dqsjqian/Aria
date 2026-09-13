#pragma once

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace aria {

enum class ListChangeKind { Insert, Remove, Replace, ItemChanged, Reset, Move };

/// An owning event in a sequential list edit stream. Indices refer to the
/// receiver's replayed mirror; the producer may already hold the batch's final
/// state. Consumers use item/snapshot, never at(index), to read event payloads.
template<typename T>
struct ListChange {
    using Snapshot = std::vector<std::shared_ptr<T>>;
    ListChangeKind kind;
    std::size_t index = 0;
    std::shared_ptr<T> item;
    std::size_t from_index = 0;
    std::shared_ptr<const Snapshot> snapshot = nullptr;

    static ListChange reset(Snapshot items) {
        return {ListChangeKind::Reset, 0, {}, 0,
                std::make_shared<const Snapshot>(std::move(items))};
    }

    static ListChange cleared() {
        static const auto empty = std::make_shared<const Snapshot>();
        return {ListChangeKind::Reset, 0, {}, 0, empty};
    }
};

} // namespace aria

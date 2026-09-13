#pragma once

#include "aria/list_change.hpp"
#include <algorithm>
#include <vector>

namespace aria::detail {

template<typename T>
void replay_list_change(std::vector<std::shared_ptr<T>>& items, const ListChange<T>& change) {
    const auto index = static_cast<std::ptrdiff_t>(change.index);
    switch (change.kind) {
    case ListChangeKind::Insert: items.insert(items.begin() + index, change.item); break;
    case ListChangeKind::Remove: items.erase(items.begin() + index); break;
    case ListChangeKind::Replace: items.at(change.index) = change.item; break;
    case ListChangeKind::Move: {
        const auto from = static_cast<std::ptrdiff_t>(change.from_index);
        if (from < index) std::rotate(items.begin() + from, items.begin() + from + 1, items.begin() + index + 1);
        else if (from > index) std::rotate(items.begin() + index, items.begin() + from, items.begin() + from + 1);
        break;
    }
    case ListChangeKind::Reset: items = *change.snapshot; break;
    case ListChangeKind::ItemChanged: break;
    }
}

} // namespace aria::detail

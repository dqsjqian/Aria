// ============================================================================
//  fuzz_observable_list_mutation_storm.cpp  (L-31)
// ----------------------------------------------------------------------------
//  Invariant under stress:
//    "ObservableList<T> emits Insert / Remove / Replace / Move events
//     in a snapshot-stable order. Observers MUST be able to track the
//     mirror of the list using only the events; after every random
//     mutation, an event-driven mirror MUST equal `list.snapshot()`."
//
//  Strategy:
//    - Random walk: push_back / insert / remove_at / remove_range /
//      replace_at / move / clear -- plus the range variants.
//    - Two parallel mirrors:
//        a) `event_mirror` updated only via the observer callback;
//        b) `truth` = list.snapshot() taken after each mutation.
//      They MUST stay equal at every single step.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/observable_list.hpp"
#include "fuzz_support.hpp"

#include <memory>
#include <vector>

using namespace aria;

TEST_CASE("L-31 fuzz: event-driven mirror tracks snapshot under random storm") {
    fuzz::Rng rng{fuzz::seed(0xC0FFEE'31)};

    ObservableList<int> list;
    std::vector<int> event_mirror;

    auto sub = list.observe([&](const ListChange<int>& ch) {
        switch (ch.kind) {
            case ListChangeKind::Insert: {
                event_mirror.insert(
                    event_mirror.begin() + static_cast<std::ptrdiff_t>(ch.index),
                    ch.item ? *ch.item : 0);
                break;
            }
            case ListChangeKind::Remove: {
                if (ch.index < event_mirror.size()) {
                    event_mirror.erase(event_mirror.begin()
                                       + static_cast<std::ptrdiff_t>(ch.index));
                }
                break;
            }
            case ListChangeKind::Replace: {
                if (ch.index < event_mirror.size()) {
                    event_mirror[ch.index] = ch.item ? *ch.item : 0;
                }
                break;
            }
            case ListChangeKind::Move: {
                if (ch.from_index < event_mirror.size()) {
                    int v = event_mirror[ch.from_index];
                    event_mirror.erase(event_mirror.begin()
                                       + static_cast<std::ptrdiff_t>(ch.from_index));
                    if (ch.index > event_mirror.size()) {
                        event_mirror.push_back(v);
                    } else {
                        event_mirror.insert(event_mirror.begin()
                                            + static_cast<std::ptrdiff_t>(ch.index),
                                            v);
                    }
                }
                break;
            }
            case ListChangeKind::Reset: {
                event_mirror.clear();
                break;
            }
            case ListChangeKind::ItemChanged:
                // value mutation; mirror stores ints copy-by-value at
                // insert time, so per-item changes are not visible
                // here -- not relevant for the structural invariant
                // we are pinning down.
                break;
        }
    });

    auto truth = [&] {
        std::vector<int> out;
        for (auto& sp : list.snapshot()) out.push_back(*sp);
        return out;
    };
    auto check_mirrors = [&](std::size_t step) {
        const auto t = truth();
        REQUIRE_MESSAGE(t.size() == event_mirror.size(),
                        "size diverged at step ", step,
                        " truth=", t.size(),
                        " mirror=", event_mirror.size());
        for (std::size_t i = 0; i < t.size(); ++i) {
            REQUIRE_MESSAGE(t[i] == event_mirror[i],
                            "value diverged at step ", step,
                            " idx=", i);
        }
    };

    for (std::size_t step = 0; step < fuzz::iters(); ++step) {
        const std::uint32_t op = rng.u32(0, 7);
        const std::size_t   n  = list.size();
        switch (op) {
            case 0: {
                list.push_back(std::make_shared<int>(static_cast<int>(rng.u32())));
                break;
            }
            case 1: {
                const std::size_t pos = n == 0 ? 0 : rng.u32(0, static_cast<std::uint32_t>(n));
                list.insert(pos, std::make_shared<int>(static_cast<int>(rng.u32())));
                break;
            }
            case 2: {
                if (n > 0) {
                    list.remove_at(rng.u32(0, static_cast<std::uint32_t>(n - 1)));
                }
                break;
            }
            case 3: {
                if (n > 0) {
                    const std::size_t pos = rng.u32(0, static_cast<std::uint32_t>(n - 1));
                    const std::size_t cnt = rng.u32(1, static_cast<std::uint32_t>(n - pos));
                    list.remove_range(pos, cnt);
                }
                break;
            }
            case 4: {
                if (n > 0) {
                    const std::size_t pos = rng.u32(0, static_cast<std::uint32_t>(n - 1));
                    list.replace_at(pos, std::make_shared<int>(static_cast<int>(rng.u32())));
                }
                break;
            }
            case 5: {
                if (n >= 2) {
                    const std::size_t from = rng.u32(0, static_cast<std::uint32_t>(n - 1));
                    const std::size_t to   = rng.u32(0, static_cast<std::uint32_t>(n - 1));
                    list.move(from, to);
                }
                break;
            }
            case 6: {
                // insert_range: 1-3 items
                const std::size_t pos = n == 0 ? 0 : rng.u32(0, static_cast<std::uint32_t>(n));
                std::vector<std::shared_ptr<int>> bucket;
                const std::uint32_t k = rng.u32(1, 3);
                for (std::uint32_t i = 0; i < k; ++i) {
                    bucket.push_back(std::make_shared<int>(static_cast<int>(rng.u32())));
                }
                list.insert_range(pos, bucket.begin(), bucket.end());
                break;
            }
            case 7: {
                // Reset is rare -- once every ~64 steps, otherwise
                // the list never reaches interesting sizes.
                if (rng.coin(1.0 / 64.0)) list.clear();
                break;
            }
        }
        check_mirrors(step);

        // Cap pathological growth so memory stays bounded across 1M iters.
        if (list.size() > 256) list.clear();
    }
}

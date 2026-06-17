// test_appkit_table_source.mm — verify ObservableTableSource<T>
// bridges aria list-source events to NSTableView correctly.
//
// This test is light-weight: we never actually display the table; we
// install the bridge against an off-screen NSTableView, fire mutations
// on the source, and assert the bridge's local snapshot stays in sync
// with the source. The native NSTableView signals (insertRowsAtIndexes:,
// moveRowAtIndex:toIndex: etc.) are exercised but their visual side
// effects are out of scope — what we pin down here is that the
// bridge's row count and `at(i)` mirror the source through every
// ListChange variant, including Move.
//
// We also feed a MappedList (derived list) through the bridge, to
// pin the contract that any `ListSourceOf<L, T>` works.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#import "aria/adapters/appkit/AppKitTableSource.hpp"

#include "aria/observable_list.hpp"
#include "aria/derived/filtered_list.hpp"
#include "aria/derived/mapped_list.hpp"

#import <Cocoa/Cocoa.h>

#include <memory>
#include <string>

namespace {

void ensure_nsapp() {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    });
}

struct Item {
    std::string title;
};

NSTableView* make_table() {
    NSTableView* tv = [[NSTableView alloc]
                          initWithFrame:NSMakeRect(0, 0, 200, 200)];
    NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:@"col"];
    [tv addTableColumn:col];
    return tv;
}

}  // namespace

TEST_CASE("AppKit table bridge: ObservableList Insert/Remove/Move/Replace") {
    ensure_nsapp();

    aria::ObservableList<Item> list;
    list.push_back(std::make_shared<Item>(Item{"alpha"}));
    list.push_back(std::make_shared<Item>(Item{"beta"}));

    NSTableView* tv = make_table();
    aria::adapters::appkit::ObservableTableSource<Item> bridge{
        tv, list,
        [](NSTableView*, NSTableColumn*, std::shared_ptr<Item>, NSInteger) -> NSView* {
            return [[NSView alloc] initWithFrame:NSZeroRect];
        }
    };

    REQUIRE(bridge.row_count() == 2);
    CHECK(bridge.at(0)->title == "alpha");
    CHECK(bridge.at(1)->title == "beta");

    list.push_back(std::make_shared<Item>(Item{"gamma"}));
    REQUIRE(bridge.row_count() == 3);
    CHECK(bridge.at(2)->title == "gamma");

    list.remove_at(0);
    REQUIRE(bridge.row_count() == 2);
    CHECK(bridge.at(0)->title == "beta");
    CHECK(bridge.at(1)->title == "gamma");

    list.replace_at(0, std::make_shared<Item>(Item{"BETA"}));
    REQUIRE(bridge.row_count() == 2);
    CHECK(bridge.at(0)->title == "BETA");

    list.move(0, 1);    // BETA, gamma  ->  gamma, BETA
    REQUIRE(bridge.row_count() == 2);
    CHECK(bridge.at(0)->title == "gamma");
    CHECK(bridge.at(1)->title == "BETA");

    list.clear();
    CHECK(bridge.row_count() == 0);
}

TEST_CASE("AppKit table bridge: drives MappedList<Source, Target>") {
    ensure_nsapp();

    struct Vm {
        std::string display;
    };

    auto src = std::make_shared<aria::ObservableList<Item>>();
    src->push_back(std::make_shared<Item>(Item{"hi"}));
    src->push_back(std::make_shared<Item>(Item{"there"}));

    auto mapped = std::make_shared<aria::MappedList<Item, Vm>>(
        src,
        [](const Item& it) {
            return std::make_shared<Vm>(Vm{"vm:" + it.title});
        });

    NSTableView* tv = make_table();
    aria::adapters::appkit::ObservableTableSource<Vm> bridge{
        tv, *mapped,
        [](NSTableView*, NSTableColumn*, std::shared_ptr<Vm>, NSInteger) -> NSView* {
            return [[NSView alloc] initWithFrame:NSZeroRect];
        }
    };

    REQUIRE(bridge.row_count() == 2);
    CHECK(bridge.at(0)->display == "vm:hi");
    CHECK(bridge.at(1)->display == "vm:there");

    // Move on the source should propagate as Move through MappedList,
    // and the bridge should preserve Target identity (no rebuild).
    auto vm0_before = bridge.at(0);
    src->move(0, 1);
    REQUIRE(bridge.row_count() == 2);
    CHECK(bridge.at(0)->display == "vm:there");
    CHECK(bridge.at(1).get() == vm0_before.get());   // identity preserved
}

TEST_CASE("AppKit table bridge: drives FilteredList") {
    ensure_nsapp();

    auto src = std::make_shared<aria::ObservableList<Item>>();
    src->push_back(std::make_shared<Item>(Item{"alpha"}));
    src->push_back(std::make_shared<Item>(Item{"beta"}));
    src->push_back(std::make_shared<Item>(Item{"alfa"}));

    auto filtered = std::make_shared<aria::FilteredList<Item>>(
        src, [](const Item& it) {
            return !it.title.empty() && it.title[0] == 'a';
        });

    NSTableView* tv = make_table();
    aria::adapters::appkit::ObservableTableSource<Item> bridge{
        tv, *filtered,
        [](NSTableView*, NSTableColumn*, std::shared_ptr<Item>, NSInteger) -> NSView* {
            return [[NSView alloc] initWithFrame:NSZeroRect];
        }
    };

    REQUIRE(bridge.row_count() == 2);
    CHECK(bridge.at(0)->title == "alpha");
    CHECK(bridge.at(1)->title == "alfa");

    // Adding a non-matching item must not enlarge the filtered view.
    src->push_back(std::make_shared<Item>(Item{"zeta"}));
    CHECK(bridge.row_count() == 2);

    // Adding a matching item appears at the right derived position.
    src->push_back(std::make_shared<Item>(Item{"aleph"}));
    REQUIRE(bridge.row_count() == 3);
    CHECK(bridge.at(2)->title == "aleph");
}

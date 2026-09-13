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
#include <thread>

@interface AriaReloadProbeTable : NSTableView
@property(nonatomic, copy) void (^onReload)(void);
@end
@implementation AriaReloadProbeTable
- (void)reloadData {
    [super reloadData];
    auto callback = self.onReload;
    self.onReload = nil;
    if (callback) callback();
}
@end

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

TEST_CASE("AppKit table bridge: source change during initial reload is observed") {
    ensure_nsapp();
    aria::ObservableList<Item> list;
    AriaReloadProbeTable* table = [[AriaReloadProbeTable alloc] initWithFrame:NSZeroRect];
    auto* source = &list;
    table.onReload = ^{ source->push_back(std::make_shared<Item>(Item{"added during reload"})); };
    aria::adapters::appkit::ObservableTableSource<Item> bridge{
        table, list, [](NSTableView*, NSTableColumn*, std::shared_ptr<Item>, NSInteger) -> NSView* { return nil; }};
    REQUIRE(bridge.row_count() == 1);
    CHECK(bridge.at(0)->title == "added during reload");
}

TEST_CASE("AppKit table bridge: queued worker change precedes main-thread replacement") {
    ensure_nsapp();
    aria::ObservableList<Item> list;
    NSTableView* table = make_table();
    aria::adapters::appkit::ObservableTableSource<Item> bridge{
        table, list, [](NSTableView*, NSTableColumn*, std::shared_ptr<Item>, NSInteger) -> NSView* { return nil; }};
    std::thread worker([&] { list.push_back(std::make_shared<Item>(Item{"old"})); });
    worker.join();
    list.replace_at(0, std::make_shared<Item>(Item{"new"}));
    NSDate* limit = [NSDate dateWithTimeIntervalSinceNow:1];
    while (bridge.row_count() == 0 && [limit timeIntervalSinceNow] > 0) {
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.001]];
    }
    REQUIRE(bridge.row_count() == 1);
    CHECK(bridge.at(0)->title == "new");
}

TEST_CASE("AppKit table bridge: Reset consumes its complete owned snapshot") {
    ensure_nsapp();
    struct Source {
        std::vector<std::shared_ptr<Item>> rows;
        aria::detail::ListSignal<Item> signal;
        auto snapshot() const { return rows; }
        std::size_t size() const { return rows.size(); }
        auto at(std::size_t index) const { return rows.at(index); }
        aria::Subscription observe(std::function<void(const aria::ListChange<Item>&)> fn) { return signal.connect(std::move(fn)); }
    } source;
    NSTableView* table = make_table();
    aria::adapters::appkit::ObservableTableSource<Item> bridge{
        table, source, [](NSTableView*, NSTableColumn*, std::shared_ptr<Item>, NSInteger) -> NSView* { return nil; }};
    source.rows = {std::make_shared<Item>(Item{"reset row"})};
    source.signal.emit(aria::ListChange<Item>::reset(source.rows));
    REQUIRE(bridge.row_count() == 1);
    CHECK(bridge.at(0) == source.rows[0]);
}

TEST_CASE("AppKit table bridge: worker teardown releases native state and captures on main") {
    ensure_nsapp();
    aria::ObservableList<Item> list;
    NSTableView* table = make_table();
    bool released_on_main = false;
    auto capture = std::shared_ptr<int>(new int(0), [&](int* value) {
        released_on_main = [NSThread isMainThread];
        delete value;
    });
    std::weak_ptr<int> weak = capture;
    using Bridge = aria::adapters::appkit::ObservableTableSource<Item>;
    auto bridge = std::make_unique<Bridge>(table, list,
        [capture = std::move(capture)](NSTableView*, NSTableColumn*, std::shared_ptr<Item>, NSInteger) -> NSView* { return nil; });
    std::thread worker([&list, owned = std::move(bridge)]() mutable {
        list.push_back(std::make_shared<Item>(Item{"queued"}));
        owned.reset();
    });
    worker.join();
    CHECK_FALSE(weak.expired());
    NSDate* limit = [NSDate dateWithTimeIntervalSinceNow:1];
    while (!weak.expired() && [limit timeIntervalSinceNow] > 0) {
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.001]];
    }
    CHECK(weak.expired());
    CHECK(released_on_main);
    CHECK((table.dataSource == nil));
    CHECK((table.delegate == nil));
}

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

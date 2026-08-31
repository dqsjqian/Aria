// test_uikit_table_source.mm — verify ObservableTableSource<T> bridges
// aria list-source events to UITableView correctly.
//
// UIKit counterpart of `test_appkit_table_source.mm`. Until now
// `UIKitTableSource.hpp` was the ONLY one of the four list/table bridges
// with no test at all (Qt6 has `test_list_model.cpp`, AppKit has
// `test_appkit_table_source.mm`, JNI has `test_jni_list_source.cpp`), so
// its row arithmetic — the part where an off-by-one silently shows the
// wrong row — was carried entirely by review.
//
// Same shape and scope as the AppKit test: we never display the table.
// The bridge is installed against an off-screen UITableView, mutations
// are fired on the source, and we assert the bridge's local snapshot
// (`row_count()` / `at(i)`) stays in lockstep through every ListChange
// variant including Move. The native UITableView calls
// (insertRowsAtIndexPaths: etc.) are exercised, but their visual side
// effects are out of scope.
//
// Derived lists are fed through the same bridge to pin the contract that
// any `ListSourceOf<L, T>` works — which is the whole "derived
// collections → UI adapter wiring" promise, and was the one platform
// where nothing verified it.
//
// Runs inside an iOS simulator (see the `uikit` CI job); the mutations
// below all happen on the main thread, so the bridge's main-queue hop is
// taken synchronously.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#import "aria/adapters/uikit/UIKitTableSource.hpp"

#include "aria/observable_list.hpp"
#include "aria/derived/filtered_list.hpp"
#include "aria/derived/mapped_list.hpp"

#import <UIKit/UIKit.h>

#include <memory>
#include <string>

namespace {

struct Item {
    std::string title;
};

UITableView* make_table() {
    return [[UITableView alloc] initWithFrame:CGRectMake(0, 0, 320, 480)
                                        style:UITableViewStylePlain];
}

/// Cell factory shared by every case — the bridge only needs it to be
/// callable; what it returns is irrelevant to snapshot bookkeeping.
::aria::adapters::uikit::ObservableTableSource<Item>::CellForRowFn cell_fn() {
    return [](UITableView*, std::shared_ptr<Item>, NSIndexPath*) -> UITableViewCell* {
        return [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                     reuseIdentifier:@"cell"];
    };
}

std::shared_ptr<Item> item(std::string t) {
    return std::make_shared<Item>(Item{std::move(t)});
}

}  // namespace

TEST_CASE("UIKit table bridge: ObservableList Insert/Remove/Replace/Move/Reset") {
    aria::ObservableList<Item> list;
    list.push_back(item("alpha"));
    list.push_back(item("beta"));

    UITableView* tv = make_table();
    ::aria::adapters::uikit::ObservableTableSource<Item> bridge{
        tv, list, cell_fn()};

    // Initial snapshot is taken in the constructor.
    REQUIRE(bridge.row_count() == 2);
    CHECK(bridge.at(0)->title == "alpha");
    CHECK(bridge.at(1)->title == "beta");

    list.push_back(item("gamma"));
    REQUIRE(bridge.row_count() == 3);
    CHECK(bridge.at(2)->title == "gamma");

    // Insert at the front must shift every following row.
    list.insert(0, item("zero"));
    REQUIRE(bridge.row_count() == 4);
    CHECK(bridge.at(0)->title == "zero");
    CHECK(bridge.at(1)->title == "alpha");

    list.remove_at(0);
    REQUIRE(bridge.row_count() == 3);
    CHECK(bridge.at(0)->title == "alpha");

    list.replace_at(0, item("ALPHA"));
    REQUIRE(bridge.row_count() == 3);
    CHECK(bridge.at(0)->title == "ALPHA");

    list.move(0, 2);   // ALPHA, beta, gamma -> beta, gamma, ALPHA
    REQUIRE(bridge.row_count() == 3);
    CHECK(bridge.at(0)->title == "beta");
    CHECK(bridge.at(1)->title == "gamma");
    CHECK(bridge.at(2)->title == "ALPHA");

    list.move(2, 0);   // and back
    CHECK(bridge.at(0)->title == "ALPHA");
    CHECK(bridge.at(1)->title == "beta");

    list.clear();
    CHECK(bridge.row_count() == 0);
}

TEST_CASE("UIKit table bridge: rows keep shared_ptr identity across a Move") {
    aria::ObservableList<Item> list;
    auto a = item("a");
    auto b = item("b");
    list.push_back(a);
    list.push_back(b);

    UITableView* tv = make_table();
    ::aria::adapters::uikit::ObservableTableSource<Item> bridge{
        tv, list, cell_fn()};

    CHECK(bridge.at(0).get() == a.get());
    CHECK(bridge.at(1).get() == b.get());

    // A Move must reorder the existing objects, not rebuild them —
    // otherwise cell reuse and selection lose their anchor.
    list.move(0, 1);
    CHECK(bridge.at(0).get() == b.get());
    CHECK(bridge.at(1).get() == a.get());
}

TEST_CASE("UIKit table bridge: out-of-range at() yields nullptr") {
    aria::ObservableList<Item> list;
    list.push_back(item("only"));

    UITableView* tv = make_table();
    ::aria::adapters::uikit::ObservableTableSource<Item> bridge{
        tv, list, cell_fn()};

    CHECK(bridge.at(0) != nullptr);
    CHECK(bridge.at(1) == nullptr);
    CHECK(bridge.at(9999) == nullptr);
}

TEST_CASE("UIKit table bridge: drives FilteredList") {
    auto src = std::make_shared<aria::ObservableList<Item>>();
    src->push_back(item("alpha"));
    src->push_back(item("beta"));
    src->push_back(item("alfa"));

    auto filtered = std::make_shared<aria::FilteredList<Item>>(
        src, [](const Item& it) { return it.title.rfind("al", 0) == 0; });

    UITableView* tv = make_table();
    ::aria::adapters::uikit::ObservableTableSource<Item> bridge{
        tv, *filtered, cell_fn()};

    REQUIRE(bridge.row_count() == 2);
    CHECK(bridge.at(0)->title == "alpha");
    CHECK(bridge.at(1)->title == "alfa");

    // A matching append arrives through the derived list, already filtered.
    src->push_back(item("also"));
    REQUIRE(bridge.row_count() == 3);
    CHECK(bridge.at(2)->title == "also");

    // A non-matching append must not reach the bridge at all.
    src->push_back(item("zeta"));
    CHECK(bridge.row_count() == 3);
}

TEST_CASE("UIKit table bridge: drives MappedList<Source, Target>") {
    struct Vm {
        std::string display;
    };

    auto src = std::make_shared<aria::ObservableList<Item>>();
    src->push_back(item("hi"));
    src->push_back(item("there"));

    auto mapped = std::make_shared<aria::MappedList<Item, Vm>>(
        src,
        [](const Item& it) { return std::make_shared<Vm>(Vm{"vm:" + it.title}); });

    UITableView* tv = make_table();
    ::aria::adapters::uikit::ObservableTableSource<Vm> bridge{
        tv, *mapped,
        [](UITableView*, std::shared_ptr<Vm>, NSIndexPath*) -> UITableViewCell* {
            return [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                         reuseIdentifier:@"vm"];
        }};

    REQUIRE(bridge.row_count() == 2);
    CHECK(bridge.at(0)->display == "vm:hi");
    CHECK(bridge.at(1)->display == "vm:there");

    // A Move on the source propagates as a Move through MappedList; the
    // bridge must preserve Target identity rather than re-mapping.
    auto vm0_before = bridge.at(0);
    src->move(0, 1);
    REQUIRE(bridge.row_count() == 2);
    CHECK(bridge.at(0)->display == "vm:there");
    CHECK(bridge.at(1).get() == vm0_before.get());
}

TEST_CASE("UIKit table bridge: teardown detaches from the table and the source") {
    aria::ObservableList<Item> list;
    list.push_back(item("a"));

    UITableView* tv = make_table();
    {
        ::aria::adapters::uikit::ObservableTableSource<Item> bridge{
            tv, list, cell_fn()};
        REQUIRE(bridge.row_count() == 1);
        // Compare as bool: doctest stringifies its operands, and casting
        // an ARC-managed id to void* needs a bridged cast.
        CHECK((tv.dataSource != nil));
    }
    // The bridge zeroes dataSource/delegate iff they are still its own
    // object, so a destroyed bridge cannot be called back into.
    CHECK((tv.dataSource == nil));
    CHECK((tv.delegate == nil));

    // Mutating the source after teardown must not crash (the
    // Subscription is released before the ObjC state is torn down).
    list.push_back(item("b"));
    CHECK(list.size() == 2);
}

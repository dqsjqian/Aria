#include <doctest/doctest.h>

#include "aria/observable_list.hpp"
#include "aria/derived/filtered_list.hpp"
#include "aria/derived/sorted_list.hpp"
#include "aria/derived/mapped_list.hpp"
#include "aria/adapters/qt6/qt_list_model_adapter.hpp"

#include <QCoreApplication>
#include <QHash>
#include <QString>
#include <QVariant>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>
#include <string>

using namespace aria;
using namespace aria::adapters::qt6;

namespace {

struct Item {
    std::string title;
    bool operator==(const Item&) const = default;
};

QHash<int, QByteArray> roles() {
    QHash<int, QByteArray> r;
    r[Qt::UserRole + 1] = "title";
    return r;
}

QVariant role_fn(const Item& it, int role) {
    if (role == Qt::DisplayRole || role == Qt::UserRole + 1)
        return QString::fromStdString(it.title);
    return {};
}

}  // namespace

TEST_CASE("ObservableListModel: starts with snapshot of source") {
    ObservableList<Item> list;
    list.push_back(std::make_shared<Item>(Item{"a"}));
    list.push_back(std::make_shared<Item>(Item{"b"}));

    ObservableListModel<Item> model{list, roles(), role_fn};
    CHECK(model.rowCount() == 2);
    CHECK(model.data(model.index(0)).toString() == "a");
    CHECK(model.data(model.index(1)).toString() == "b");
}

TEST_CASE("ObservableListModel: insert appears in model") {
    ObservableList<Item> list;
    ObservableListModel<Item> model{list, roles(), role_fn};
    CHECK(model.rowCount() == 0);

    list.push_back(std::make_shared<Item>(Item{"hello"}));
    CHECK(model.rowCount() == 1);
    CHECK(model.data(model.index(0)).toString() == "hello");
}

TEST_CASE("ObservableListModel: remove_at shrinks the model") {
    ObservableList<Item> list;
    list.push_back(std::make_shared<Item>(Item{"a"}));
    list.push_back(std::make_shared<Item>(Item{"b"}));
    list.push_back(std::make_shared<Item>(Item{"c"}));

    ObservableListModel<Item> model{list, roles(), role_fn};
    CHECK(model.rowCount() == 3);

    list.remove_at(1);
    CHECK(model.rowCount() == 2);
    CHECK(model.data(model.index(0)).toString() == "a");
    CHECK(model.data(model.index(1)).toString() == "c");
}

TEST_CASE("ObservableListModel: clear resets to empty") {
    ObservableList<Item> list;
    list.push_back(std::make_shared<Item>(Item{"a"}));
    list.push_back(std::make_shared<Item>(Item{"b"}));

    ObservableListModel<Item> model{list, roles(), role_fn};
    list.clear();
    CHECK(model.rowCount() == 0);
}

// ═══════════════════════════════════════════════════════════════════════
//  Snapshot consistency under rapid mutation
//
//  Regression for the subtle race where `apply_change_` used to call
//  `list_->at(ch.index)` inside the queued lambda. By the time that
//  lambda actually ran on the owning thread, the source list may have
//  moved on -- so the adapter's internal `snapshot_` could diverge
//  from the source. The fix captures the item at emit time and passes
//  it in; this test pins down that contract even on the
//  same-thread-synchronous path.
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("ObservableListModel: rapid back-to-back inserts preserve order") {
    ObservableList<Item> list;
    ObservableListModel<Item> model{list, roles(), role_fn};

    // Fire a burst of inserts at the same index. Before the fix, the
    // queued lambda for the FIRST insert would call list_->at(0) after
    // the SECOND insert had already landed, fetching the wrong item.
    list.insert(0, std::make_shared<Item>(Item{"A"}));
    list.insert(0, std::make_shared<Item>(Item{"B"}));
    list.insert(0, std::make_shared<Item>(Item{"C"}));

    // Source list order: C, B, A (every insert prepended).
    REQUIRE(model.rowCount() == 3);
    CHECK(model.data(model.index(0)).toString() == "C");
    CHECK(model.data(model.index(1)).toString() == "B");
    CHECK(model.data(model.index(2)).toString() == "A");
}

TEST_CASE("ObservableListModel: insert then remove then insert stays coherent") {
    ObservableList<Item> list;
    ObservableListModel<Item> model{list, roles(), role_fn};

    list.push_back(std::make_shared<Item>(Item{"first"}));
    list.push_back(std::make_shared<Item>(Item{"second"}));
    list.remove_at(0);
    list.push_back(std::make_shared<Item>(Item{"third"}));

    // Expected: ["second", "third"].
    REQUIRE(model.rowCount() == 2);
    CHECK(model.data(model.index(0)).toString() == "second");
    CHECK(model.data(model.index(1)).toString() == "third");
}

TEST_CASE("ObservableListModel: move reorders the model view") {
    ObservableList<Item> list;
    list.push_back(std::make_shared<Item>(Item{"a"}));
    list.push_back(std::make_shared<Item>(Item{"b"}));
    list.push_back(std::make_shared<Item>(Item{"c"}));

    ObservableListModel<Item> model{list, roles(), role_fn};
    REQUIRE(model.rowCount() == 3);

    list.move(0, 2);    // a,b,c  ->  b,c,a

    CHECK(model.data(model.index(0)).toString() == "b");
    CHECK(model.data(model.index(1)).toString() == "c");
    CHECK(model.data(model.index(2)).toString() == "a");
}

// ═══════════════════════════════════════════════════════════════════════
//  Generic ListSource binding
//
//  ObservableListModel now accepts FilteredList / SortedList / MappedList
//  via the `aria::ListSource` concept, not just ObservableList. The
//  Insert / Remove / Replace / Move / Reset event vocabulary is the
//  same across all four, so the adapter's bridging logic does not
//  duplicate per source type.
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("ObservableListModel: drives FilteredList directly") {
    auto src = std::make_shared<ObservableList<Item>>();
    src->push_back(std::make_shared<Item>(Item{"alpha"}));
    src->push_back(std::make_shared<Item>(Item{"beta"}));
    src->push_back(std::make_shared<Item>(Item{"alfa"}));

    auto filtered = std::make_shared<FilteredList<Item>>(
        src, [](const Item& it) { return !it.title.empty() && it.title[0] == 'a'; });

    ObservableListModel<Item> model{*filtered, roles(), role_fn};
    REQUIRE(model.rowCount() == 2);
    CHECK(model.data(model.index(0)).toString() == "alpha");
    CHECK(model.data(model.index(1)).toString() == "alfa");

    // Adding a non-matching item must NOT inflate the model.
    src->push_back(std::make_shared<Item>(Item{"gamma"}));
    CHECK(model.rowCount() == 2);

    // Adding a matching item MUST insert at the right derived row.
    src->push_back(std::make_shared<Item>(Item{"aleph"}));
    REQUIRE(model.rowCount() == 3);
    CHECK(model.data(model.index(2)).toString() == "aleph");
}

TEST_CASE("ObservableListModel: drives SortedList — Move is bridged") {
    auto src = std::make_shared<ObservableList<Item>>();
    src->push_back(std::make_shared<Item>(Item{"c"}));
    src->push_back(std::make_shared<Item>(Item{"a"}));
    src->push_back(std::make_shared<Item>(Item{"b"}));

    auto sorted = std::make_shared<SortedList<Item>>(
        src, [](const Item& l, const Item& r) { return l.title < r.title; });

    ObservableListModel<Item> model{*sorted, roles(), role_fn};
    REQUIRE(model.rowCount() == 3);
    CHECK(model.data(model.index(0)).toString() == "a");
    CHECK(model.data(model.index(1)).toString() == "b");
    CHECK(model.data(model.index(2)).toString() == "c");

    // Pushing "aa" lands between "a" and "b" in the sorted view; the
    // adapter must end up with that order, regardless of whether the
    // SortedList delivered the change as Insert or as Insert+Move.
    src->push_back(std::make_shared<Item>(Item{"aa"}));
    REQUIRE(model.rowCount() == 4);
    CHECK(model.data(model.index(0)).toString() == "a");
    CHECK(model.data(model.index(1)).toString() == "aa");
    CHECK(model.data(model.index(2)).toString() == "b");
    CHECK(model.data(model.index(3)).toString() == "c");
}

TEST_CASE("ObservableListModel: drives MappedList — Target identity preserved") {
    struct Vm {
        std::string display;
    };
    auto roles_vm = []() {
        QHash<int, QByteArray> r;
        r[Qt::UserRole + 1] = "display";
        return r;
    };
    auto role_fn_vm = [](const Vm& v, int role) -> QVariant {
        if (role == Qt::DisplayRole || role == Qt::UserRole + 1)
            return QString::fromStdString(v.display);
        return {};
    };

    auto src = std::make_shared<ObservableList<Item>>();
    src->push_back(std::make_shared<Item>(Item{"hi"}));
    src->push_back(std::make_shared<Item>(Item{"there"}));

    auto mapped = std::make_shared<MappedList<Item, Vm>>(
        src,
        [](const Item& it) {
            return std::make_shared<Vm>(Vm{"vm:" + it.title});
        });

    ObservableListModel<Vm> model{*mapped, roles_vm(), role_fn_vm};
    REQUIRE(model.rowCount() == 2);
    CHECK(model.data(model.index(0)).toString() == "vm:hi");
    CHECK(model.data(model.index(1)).toString() == "vm:there");

    // A Move on the source propagates through the MappedList as a
    // Move, and the model adapter bridges it to begin/endMoveRows
    // without rebuilding the rows (Target identity is preserved).
    src->move(0, 1);
    REQUIRE(model.rowCount() == 2);
    CHECK(model.data(model.index(0)).toString() == "vm:there");
    CHECK(model.data(model.index(1)).toString() == "vm:hi");
}

namespace {
// Models an observer already copied into an upstream delivery queue before
// its subscription is released, and a source that commits a batch up front.
struct QueuedListSource {
    using value_type = Item;
    std::vector<std::shared_ptr<Item>> rows;
    std::function<void(const ListChange<Item>&)> callback;
    Subscription observe(std::function<void(const ListChange<Item>&)> fn) {
        callback = std::move(fn);
        return {};
    }
    std::size_t size() const { return rows.size(); }
    std::shared_ptr<Item> at(std::size_t index) const { return rows.at(index); }
    auto snapshot() const { return rows; }
};
int model_role_failures = 0;
void model_failure_sink(const CallbackFailure& failure) {
    if (failure.category == "qt.list_model.role") ++model_role_failures;
}
}

TEST_CASE("ObservableListModel: in-flight source callbacks cannot access a destroyed model") {
    QueuedListSource source;
    auto model = std::make_unique<ObservableListModel<Item>>(source, roles(), role_fn);
    auto in_flight = source.callback;
    model.reset();
    CHECK_NOTHROW(in_flight({ListChangeKind::Insert, 0, std::make_shared<Item>(Item{"late"})}));
}

TEST_CASE("ObservableListModel: queued Reset replays its own snapshot before later inserts") {
    QueuedListSource source;
    source.rows.push_back(std::make_shared<Item>(Item{"old"}));
    ObservableListModel<Item> model{source, roles(), role_fn};
    std::thread producer([&] {
        source.rows.clear();
        source.callback(ListChange<Item>::cleared());
        auto next = std::make_shared<Item>(Item{"new"});
        source.rows.push_back(next);
        source.callback({ListChangeKind::Insert, 0, next});
    });
    producer.join();
    CHECK(model.rowCount() == 1); // queued deliveries have not run yet
    QCoreApplication::processEvents();
    REQUIRE(model.rowCount() == 1);
    CHECK(model.data(model.index(0)).toString() == "new");
}

TEST_CASE("ObservableListModel: committed batches use event items rather than source indexes") {
    QueuedListSource source;
    ObservableListModel<Item> model{source, roles(), role_fn};
    auto first = std::make_shared<Item>(Item{"first"});
    auto second = std::make_shared<Item>(Item{"second"});
    source.rows = {first, second};
    source.callback({ListChangeKind::Insert, 0, second});
    source.callback({ListChangeKind::Insert, 0, first});
    REQUIRE(model.rowCount() == 2);
    CHECK(model.data(model.index(0)).toString() == "first");
    CHECK(model.data(model.index(1)).toString() == "second");
}

TEST_CASE("ObservableListModel: role failures stay inside the Qt callback boundary") {
    QueuedListSource source;
    source.rows.push_back(std::make_shared<Item>(Item{"row"}));
    ObservableListModel<Item> model{source, roles(), [](const Item&, int) -> QVariant {
        throw std::runtime_error("projection failure");
    }};
    model_role_failures = 0;
    auto previous = set_callback_failure_sink(model_failure_sink);
    QVariant value;
    CHECK_NOTHROW(value = model.data(model.index(0)));
    set_callback_failure_sink(previous);
    CHECK_FALSE(value.isValid());
    CHECK(model_role_failures == 1);
}

TEST_CASE("ObservableListModel: role and row stay alive if projection destroys the model") {
    QueuedListSource source;
    source.rows.push_back(std::make_shared<Item>(Item{"row"}));
    std::unique_ptr<ObservableListModel<Item>> model;
    auto held = std::make_shared<int>(42);
    model = std::make_unique<ObservableListModel<Item>>(source, roles(),
        [&, held](const Item& item, int) {
            model.reset();
            return QString::fromStdString(item.title) + QString::number(*held);
        });
    auto value = model->data(model->index(0));
    CHECK_FALSE(model);
    CHECK(value.toString() == "row42");
    CHECK(held.use_count() == 1);
}

TEST_CASE("ObservableListModel: producer delivery racing owner-thread destruction is safe") {
    for (int round = 0; round < 64; ++round) {
        QueuedListSource source;
        auto model = std::make_unique<ObservableListModel<Item>>(source, roles(), role_fn);
        auto in_flight = source.callback;
        std::atomic<bool> ready{false};
        std::thread producer([&] {
            ready.store(true, std::memory_order_release);
            for (int i = 0; i < 20; ++i)
                in_flight({ListChangeKind::Insert, static_cast<std::size_t>(i),
                           std::make_shared<Item>(Item{"row"})});
        });
        while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
        model.reset();
        producer.join();
        QCoreApplication::processEvents();
    }
    CHECK(true); // ASan/UBSan cover receiver and queued-payload lifetimes.
}

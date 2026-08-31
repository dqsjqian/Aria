// test_jni_list_source.cpp — behavioural test for the RecyclerView bridge.
//
// `JniListSource<T>` carries the snapshot and the diffing and has no
// `<jni.h>` dependency, precisely so this file can run on the build host
// with no JVM. That matters: the rest of the JNI adapter can only be
// static_asserted (constructing a JniView needs a live JNIEnv*), and a
// list bridge whose row arithmetic nobody executes is how off-by-one row
// bugs reach a device.
//
// What is pinned here:
//   * every ListChange variant maps to the documented RecyclerView call;
//   * the bridge's own row view (`item_count` / `at`) stays in lockstep
//     with the source through all of them;
//   * item IDENTITY survives — the whole point of the change, since the
//     workaround it replaces joined rows into one string;
//   * derived lists (FilteredList / MappedList) work, i.e. the bridge
//     really consumes `ListSourceOf<L, T>` and not just ObservableList;
//   * out-of-range and no-op changes raise nothing.
//
// The JNI half (`JniRecyclerNotifier`) needs a real VM and is covered by
// the compile-time contract test plus on-device instrumentation.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "aria/adapters/jni/JniListSource.hpp"

#include "aria/observable_list.hpp"
#include "aria/derived/filtered_list.hpp"
#include "aria/derived/mapped_list.hpp"

#include <memory>
#include <string>
#include <vector>

using ::aria::adapters::jni::JniListSource;
using ::aria::adapters::jni::RecyclerNotification;
using ::aria::adapters::jni::RecyclerNotify;

namespace {

struct Item {
    std::string title;
};

/// Records what a managed RecyclerView.Adapter would have been told.
struct NotifyLog {
    std::vector<RecyclerNotification> events;

    [[nodiscard]] auto sink() {
        return [this](const RecyclerNotification& n) { events.push_back(n); };
    }
    [[nodiscard]] std::size_t count() const { return events.size(); }
    [[nodiscard]] const RecyclerNotification& last() const {
        return events.back();
    }
    void clear() { events.clear(); }
};

std::shared_ptr<Item> item(std::string t) {
    return std::make_shared<Item>(Item{std::move(t)});
}

/// Titles as the bridge currently sees them — the managed side would
/// render exactly this, in this order.
std::vector<std::string> rows_of(const JniListSource<Item>& src) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < src.item_count(); ++i) {
        auto row = src.at(i);
        out.push_back(row ? row->title : std::string{"<null>"});
    }
    return out;
}

}  // namespace

TEST_CASE("JNI list: initial snapshot is taken before any notification") {
    aria::ObservableList<Item> list;
    list.push_back(item("alpha"));
    list.push_back(item("beta"));

    NotifyLog log;
    JniListSource<Item> src{list, log.sink()};

    // Attaching must not spam the managed adapter — getItemCount() is
    // already correct, so a leading notifyDataSetChanged would be noise.
    CHECK(log.count() == 0);
    CHECK(src.item_count() == 2);
    CHECK(rows_of(src) == std::vector<std::string>{"alpha", "beta"});
}

TEST_CASE("JNI list: Insert maps to notifyItemInserted at the right row") {
    aria::ObservableList<Item> list;
    list.push_back(item("alpha"));

    NotifyLog log;
    JniListSource<Item> src{list, log.sink()};

    list.push_back(item("beta"));
    REQUIRE(log.count() == 1);
    CHECK(log.last().kind == RecyclerNotify::ItemInserted);
    CHECK(log.last().position == 1);

    list.insert(0, item("zero"));
    REQUIRE(log.count() == 2);
    CHECK(log.last().kind == RecyclerNotify::ItemInserted);
    CHECK(log.last().position == 0);

    CHECK(rows_of(src) == std::vector<std::string>{"zero", "alpha", "beta"});
}

TEST_CASE("JNI list: Remove maps to notifyItemRemoved and shifts rows") {
    aria::ObservableList<Item> list;
    list.push_back(item("a"));
    list.push_back(item("b"));
    list.push_back(item("c"));

    NotifyLog log;
    JniListSource<Item> src{list, log.sink()};

    list.remove_at(1);
    REQUIRE(log.count() == 1);
    CHECK(log.last().kind == RecyclerNotify::ItemRemoved);
    CHECK(log.last().position == 1);
    CHECK(rows_of(src) == std::vector<std::string>{"a", "c"});
}

TEST_CASE("JNI list: Move reports raw (from, to) with no Qt-style adjustment") {
    aria::ObservableList<Item> list;
    list.push_back(item("a"));
    list.push_back(item("b"));
    list.push_back(item("c"));

    NotifyLog log;
    JniListSource<Item> src{list, log.sink()};

    // Downward move: RecyclerView's notifyItemMoved takes the plain
    // destination index, unlike QAbstractItemModel::beginMoveRows which
    // needs `to + 1`. Getting this wrong is the classic port bug.
    list.move(0, 2);
    REQUIRE(log.count() == 1);
    CHECK(log.last().kind == RecyclerNotify::ItemMoved);
    CHECK(log.last().from_position == 0);
    CHECK(log.last().position == 2);
    CHECK(rows_of(src) == std::vector<std::string>{"b", "c", "a"});

    // Upward move.
    list.move(2, 0);
    REQUIRE(log.count() == 2);
    CHECK(log.last().from_position == 2);
    CHECK(log.last().position == 0);
    CHECK(rows_of(src) == std::vector<std::string>{"a", "b", "c"});
}

TEST_CASE("JNI list: Replace maps to notifyItemChanged and swaps identity") {
    aria::ObservableList<Item> list;
    auto first = item("old");
    list.push_back(first);

    NotifyLog log;
    JniListSource<Item> src{list, log.sink()};

    auto replacement = item("new");
    list.replace_at(0, replacement);

    REQUIRE(log.count() == 1);
    CHECK(log.last().kind == RecyclerNotify::ItemChanged);
    CHECK(log.last().position == 0);
    // Identity, not just text: the bridge must hand the managed side the
    // very object the source now holds.
    CHECK(src.at(0).get() == replacement.get());
}

TEST_CASE("JNI list: Reset maps to notifyDataSetChanged and empties rows") {
    aria::ObservableList<Item> list;
    list.push_back(item("a"));
    list.push_back(item("b"));

    NotifyLog log;
    JniListSource<Item> src{list, log.sink()};

    list.clear();
    REQUIRE(log.count() == 1);
    CHECK(log.last().kind == RecyclerNotify::DataSetChanged);
    CHECK(src.item_count() == 0);
}

TEST_CASE("JNI list: rows preserve shared_ptr identity, not stringified text") {
    // The workaround this bridge replaces joined rows with "\n" and split
    // them in Kotlin, which loses identity and therefore selection and
    // per-row diffing. Pin that identity survives every row.
    aria::ObservableList<Item> list;
    auto a = item("a");
    auto b = item("b");
    list.push_back(a);
    list.push_back(b);

    NotifyLog log;
    JniListSource<Item> src{list, log.sink()};

    CHECK(src.at(0).get() == a.get());
    CHECK(src.at(1).get() == b.get());

    list.move(0, 1);
    CHECK(src.at(0).get() == b.get());
    CHECK(src.at(1).get() == a.get());
}

TEST_CASE("JNI list: out-of-range at() yields nullptr instead of throwing") {
    aria::ObservableList<Item> list;
    list.push_back(item("only"));

    NotifyLog log;
    JniListSource<Item> src{list, log.sink()};

    // The managed side may ask about a row a pending notification has
    // already removed; that must not throw across the JNI boundary.
    CHECK(src.at(0) != nullptr);
    CHECK(src.at(1) == nullptr);
    CHECK(src.at(9999) == nullptr);
}

TEST_CASE("JNI list: a self-move raises no notification") {
    aria::ObservableList<Item> list;
    list.push_back(item("a"));
    list.push_back(item("b"));

    NotifyLog log;
    JniListSource<Item> src{list, log.sink()};

    list.move(1, 1);
    CHECK(log.count() == 0);
    CHECK(rows_of(src) == std::vector<std::string>{"a", "b"});
}

TEST_CASE("JNI list: reload() resyncs and raises one notifyDataSetChanged") {
    aria::ObservableList<Item> list;
    list.push_back(item("a"));

    NotifyLog log;
    JniListSource<Item> src{list, log.sink()};
    log.clear();

    src.reload();
    REQUIRE(log.count() == 1);
    CHECK(log.last().kind == RecyclerNotify::DataSetChanged);
    CHECK(rows_of(src) == std::vector<std::string>{"a"});
}

TEST_CASE("JNI list: consumes a FilteredList through the ListSource concept") {
    auto backing = std::make_shared<aria::ObservableList<Item>>();
    backing->push_back(item("keep-1"));
    backing->push_back(item("drop"));
    backing->push_back(item("keep-2"));

    aria::FilteredList<Item> visible{backing, [](const Item& i) {
        return i.title.rfind("keep", 0) == 0;
    }};

    NotifyLog log;
    JniListSource<Item> src{visible, log.sink()};
    CHECK(rows_of(src) == std::vector<std::string>{"keep-1", "keep-2"});

    // A change to the backing list must reach the bridge through the
    // derived list's own ListChange stream, already filtered.
    backing->push_back(item("keep-3"));
    CHECK(rows_of(src)
          == std::vector<std::string>{"keep-1", "keep-2", "keep-3"});
    CHECK(log.count() >= 1);
    CHECK(log.last().kind == RecyclerNotify::ItemInserted);
}

TEST_CASE("JNI list: consumes a MappedList whose element type differs") {
    auto backing = std::make_shared<aria::ObservableList<Item>>();
    backing->push_back(item("alpha"));

    // Target type is std::string, so this also pins that the bridge's `T`
    // follows the derived list's element type rather than the source's.
    aria::MappedList<Item, std::string> titles{
        backing, [](const Item& i) { return std::make_shared<std::string>(i.title); }};

    NotifyLog log;
    JniListSource<std::string> src{titles, log.sink()};
    REQUIRE(src.item_count() == 1);
    CHECK(*src.at(0) == "alpha");

    backing->push_back(item("beta"));
    REQUIRE(log.count() >= 1);
    CHECK(log.last().kind == RecyclerNotify::ItemInserted);
    REQUIRE(src.item_count() == 2);
    CHECK(*src.at(1) == "beta");
}

TEST_CASE("JNI list: destruction detaches before the sink can be reached") {
    aria::ObservableList<Item> list;
    list.push_back(item("a"));

    NotifyLog log;
    {
        JniListSource<Item> src{list, log.sink()};
        list.push_back(item("b"));
        CHECK(log.count() == 1);
    }
    // The bridge is gone; further source mutations must not reach the
    // sink (its captured NotifyLog is still alive, so a missed detach
    // would show up as a growing log rather than as a crash).
    list.push_back(item("c"));
    CHECK(log.count() == 1);
}

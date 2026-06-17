#include <doctest/doctest.h>

#include "aria/derived/mapped_list.hpp"
#include "aria/observable_list.hpp"
#include "aria/property.hpp"

#include <atomic>
#include <memory>
#include <string>

using namespace aria;

namespace {

struct Person {
    std::string name;
    int         age;
};

// VM-like Target that wraps a Person. Tracks per-instance construction
// so tests can assert mapper-invocation vs identity-preservation.
struct PersonVM {
    static std::atomic<int> live_count;

    std::string display;
    int         age_at_construction;

    PersonVM(const Person& p)
        : display(p.name + " (" + std::to_string(p.age) + ")"),
          age_at_construction(p.age)
    {
        ++live_count;
    }
    ~PersonVM() { --live_count; }
};
std::atomic<int> PersonVM::live_count{0};

// Reactive Person, for ItemChanged tests.
struct ReactivePerson {
    Property<std::string> name{""};
    Property<int>         age{0};

    [[nodiscard]] Subscription on_changed(std::function<void(const ReactivePerson&)> fn) {
        return name.bind([this, f = fn](const std::string&){ f(*this); });
    }
};

auto make_p(std::string n, int a) {
    return std::make_shared<Person>(Person{std::move(n), a});
}

template<typename S, typename T>
struct EventLog {
    std::vector<ListChange<T>> events;
    Subscription sub;

    explicit EventLog(MappedList<S, T>& l) {
        sub = l.observe([this](const ListChange<T>& ch) {
            events.push_back(ch);
        });
    }
};

}  // namespace

TEST_CASE("MappedList: construction maps every source item once") {
    const int baseline = PersonVM::live_count.load();
    auto src = std::make_shared<ObservableList<Person>>();
    src->push_back(make_p("alice", 30));
    src->push_back(make_p("bob",   25));

    MappedList<Person, PersonVM> mapped{src,
        [](const Person& p) { return std::make_shared<PersonVM>(p); }};

    CHECK(mapped.size() == 2);
    CHECK(mapped.at(0)->display == "alice (30)");
    CHECK(mapped.at(1)->display == "bob (25)");
    CHECK(PersonVM::live_count.load() == baseline + 2);
}

TEST_CASE("MappedList: empty source yields empty derived") {
    auto src = std::make_shared<ObservableList<Person>>();
    MappedList<Person, PersonVM> mapped{src,
        [](const Person& p) { return std::make_shared<PersonVM>(p); }};
    CHECK(mapped.empty());
}

TEST_CASE("MappedList: source Insert maps the new item and emits Insert") {
    auto src = std::make_shared<ObservableList<Person>>();
    MappedList<Person, PersonVM> mapped{src,
        [](const Person& p) { return std::make_shared<PersonVM>(p); }};
    EventLog<Person, PersonVM> log{mapped};

    src->push_back(make_p("carol", 40));

    CHECK(mapped.size() == 1);
    CHECK(mapped.at(0)->display == "carol (40)");
    REQUIRE(log.events.size() == 1);
    CHECK(log.events[0].kind == ListChangeKind::Insert);
    CHECK(log.events[0].index == 0);
    CHECK(log.events[0].item == mapped.at(0).get());
}

TEST_CASE("MappedList: source Remove emits Remove and drops the target slot") {
    auto src = std::make_shared<ObservableList<Person>>();
    src->push_back(make_p("alice", 30));
    src->push_back(make_p("bob",   25));

    const int baseline = PersonVM::live_count.load();
    MappedList<Person, PersonVM> mapped{src,
        [](const Person& p) { return std::make_shared<PersonVM>(p); }};
    EventLog<Person, PersonVM> log{mapped};

    // Hold a weak ref to what we're about to remove — after the
    // remove, the derived list should not keep it alive.
    std::weak_ptr<PersonVM> weak_vm = mapped.at(0);

    src->remove_at(0);

    REQUIRE(log.events.size() == 1);
    CHECK(log.events[0].kind == ListChangeKind::Remove);
    CHECK(log.events[0].index == 0);
    CHECK(mapped.size() == 1);
    CHECK(weak_vm.expired());  // slot erase drops the last strong ref
    CHECK(PersonVM::live_count.load() == baseline + 1);
}

TEST_CASE("MappedList: source Replace refreshes the mapped target") {
    auto src = std::make_shared<ObservableList<Person>>();
    src->push_back(make_p("alice", 30));

    MappedList<Person, PersonVM> mapped{src,
        [](const Person& p) { return std::make_shared<PersonVM>(p); }};
    EventLog<Person, PersonVM> log{mapped};

    auto before = mapped.at(0);
    src->replace_at(0, make_p("alice", 31));  // birthday!

    REQUIRE(log.events.size() == 1);
    CHECK(log.events[0].kind == ListChangeKind::Replace);
    CHECK(log.events[0].index == 0);
    CHECK(mapped.at(0).get() != before.get());
    CHECK(mapped.at(0)->display == "alice (31)");
}

TEST_CASE("MappedList: ItemChanged preserves Target identity by default") {
    auto src = std::make_shared<ObservableList<ReactivePerson>>();
    auto rp = std::make_shared<ReactivePerson>();
    rp->name = "alice";
    rp->age  = 30;
    src->push_back(rp);

    // Target stores an initial age; if remap_on_change=false, even
    // after rp->name changes, the Target snapshot stays.
    struct Snapshot {
        std::string original_name;
    };
    MappedList<ReactivePerson, Snapshot> mapped{src,
        [](const ReactivePerson& p) {
            return std::make_shared<Snapshot>(Snapshot{p.name.get()});
        }};
    EventLog<ReactivePerson, Snapshot> log{mapped};

    auto t_before = mapped.at(0);
    rp->name = "alice2";

    REQUIRE(log.events.size() == 1);
    CHECK(log.events[0].kind == ListChangeKind::ItemChanged);
    CHECK(mapped.at(0).get() == t_before.get());  // identity preserved
    CHECK(mapped.at(0)->original_name == "alice");  // stale snapshot, by design
}

TEST_CASE("MappedList: ItemChanged with remap_on_change=true re-runs mapper") {
    auto src = std::make_shared<ObservableList<ReactivePerson>>();
    auto rp = std::make_shared<ReactivePerson>();
    rp->name = "alice";
    src->push_back(rp);

    struct Snapshot { std::string n; };
    MappedList<ReactivePerson, Snapshot> mapped{src,
        [](const ReactivePerson& p) {
            return std::make_shared<Snapshot>(Snapshot{p.name.get()});
        },
        /*remap_on_change=*/true};
    EventLog<ReactivePerson, Snapshot> log{mapped};

    auto t_before = mapped.at(0);
    rp->name = "alice2";

    REQUIRE(log.events.size() == 1);
    CHECK(log.events[0].kind == ListChangeKind::ItemChanged);
    CHECK(mapped.at(0).get() != t_before.get());  // identity CHANGED
    CHECK(mapped.at(0)->n == "alice2");
}

TEST_CASE("MappedList: source Move emits Move with same Target identity") {
    auto src = std::make_shared<ObservableList<Person>>();
    src->push_back(make_p("alice", 30));
    src->push_back(make_p("bob",   25));
    src->push_back(make_p("carol", 40));

    MappedList<Person, PersonVM> mapped{src,
        [](const Person& p) { return std::make_shared<PersonVM>(p); }};
    EventLog<Person, PersonVM> log{mapped};

    auto t_alice = mapped.at(0);
    src->move(0, 2);  // alice → tail

    REQUIRE(log.events.size() == 1);
    CHECK(log.events[0].kind == ListChangeKind::Move);
    CHECK(log.events[0].from_index == 0);
    CHECK(log.events[0].index == 2);
    CHECK(mapped.at(2).get() == t_alice.get());
}

TEST_CASE("MappedList: source Reset rebuilds and emits Reset") {
    auto src = std::make_shared<ObservableList<Person>>();
    src->push_back(make_p("alice", 30));
    src->push_back(make_p("bob",   25));

    MappedList<Person, PersonVM> mapped{src,
        [](const Person& p) { return std::make_shared<PersonVM>(p); }};
    EventLog<Person, PersonVM> log{mapped};

    src->clear();

    REQUIRE(log.events.size() == 1);
    CHECK(log.events[0].kind == ListChangeKind::Reset);
    CHECK(mapped.empty());
}

TEST_CASE("MappedList: destruction during pending source listener is safe") {
    auto src = std::make_shared<ObservableList<Person>>();
    src->push_back(make_p("alice", 30));

    {
        MappedList<Person, PersonVM> mapped{src,
            [](const Person& p) { return std::make_shared<PersonVM>(p); }};
        CHECK(mapped.size() == 1);
    }
    src->push_back(make_p("bob", 25));
    CHECK(src->size() == 2);
}

TEST_CASE("MappedList: multi-observer parity") {
    auto src = std::make_shared<ObservableList<Person>>();
    MappedList<Person, PersonVM> mapped{src,
        [](const Person& p) { return std::make_shared<PersonVM>(p); }};
    EventLog<Person, PersonVM> a{mapped};
    EventLog<Person, PersonVM> b{mapped};

    src->push_back(make_p("x", 1));
    src->push_back(make_p("y", 2));

    REQUIRE(a.events.size() == 2);
    REQUIRE(b.events.size() == 2);
    for (std::size_t i = 0; i < a.events.size(); ++i) {
        CHECK(a.events[i].kind == b.events[i].kind);
        CHECK(a.events[i].index == b.events[i].index);
    }
}

TEST_CASE("MappedList: mapper invocation count matches new-item count") {
    int invocations = 0;
    auto src = std::make_shared<ObservableList<Person>>();
    src->push_back(make_p("a", 1));
    src->push_back(make_p("b", 2));

    MappedList<Person, PersonVM> mapped{src,
        [&](const Person& p) {
            ++invocations;
            return std::make_shared<PersonVM>(p);
        }};
    CHECK(invocations == 2);

    // push_back → +1
    src->push_back(make_p("c", 3));
    CHECK(invocations == 3);

    // move → 0 (same cached target)
    src->move(0, 2);
    CHECK(invocations == 3);

    // remove → 0
    src->remove_at(0);
    CHECK(invocations == 3);

    // replace → +1
    src->replace_at(0, make_p("z", 99));
    CHECK(invocations == 4);

    // clear → 0 invocations (reset with empty source)
    src->clear();
    CHECK(invocations == 4);
}

#include <doctest/doctest.h>

#include "aria/runtime/container.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace aria::runtime;

namespace {

struct ILogger {
    virtual ~ILogger() = default;
    virtual std::string log(const std::string&) = 0;
};

struct ConsoleLogger : ILogger {
    std::string log(const std::string& m) override { return "[console] " + m; }
};

struct FileLogger : ILogger {
    std::string log(const std::string& m) override { return "[file] " + m; }
};

// ── L-40 teardown-order fixtures ────────────────────────────────────────
//
// Each service appends its own name on destruction, so a test can assert
// the exact teardown sequence rather than merely "everything went away".

struct Recorder {
    std::vector<std::string> destroyed;
};

template<int Tag>
struct Tracked {
    Recorder*   rec;
    std::string name;
    Tracked(Recorder* r, std::string n) : rec(r), name(std::move(n)) {}
    virtual ~Tracked() { rec->destroyed.push_back(name); }
};

using IDatabase = Tracked<1>;   // provider — registered first, dies last
using ICache    = Tracked<2>;   // consumer — registered last, dies first

/// Re-enters the container from its own destructor. Under a teardown that
/// destroys values while holding the container mutex this self-deadlocks;
/// the contract requires the value to die outside the lock.
struct SelfQuerying {
    Container* owner;
    bool*      saw_container;
    ~SelfQuerying() { *saw_container = owner->has<ILogger>(); }
};

}  // namespace

TEST_CASE("Container: singleton resolves same instance") {
    Container c;
    c.register_singleton<ILogger, ConsoleLogger>();
    auto a = c.resolve<ILogger>();
    auto b = c.resolve<ILogger>();
    CHECK(a.get() == b.get());
    CHECK(a->log("hi") == "[console] hi");
}

TEST_CASE("Container: register_instance uses provided shared_ptr") {
    Container c;
    auto provided = std::make_shared<FileLogger>();
    c.register_instance<ILogger>(provided);
    auto resolved = c.resolve<ILogger>();
    CHECK(resolved.get() == provided.get());
    CHECK(resolved->log("x") == "[file] x");
}

TEST_CASE("Container: factory yields fresh instance each time") {
    Container c;
    int call_count = 0;
    c.register_factory<ILogger>([&]() {
        ++call_count;
        return std::make_shared<ConsoleLogger>();
    });
    auto a = c.resolve<ILogger>();
    auto b = c.resolve<ILogger>();
    CHECK(call_count == 2);
    CHECK(a.get() != b.get());
}

TEST_CASE("Container: unregistered type throws") {
    Container c;
    CHECK_THROWS_AS((void)c.resolve<ILogger>(), std::runtime_error);
}

TEST_CASE("Container: has() reports registration state") {
    Container c;
    CHECK_FALSE(c.has<ILogger>());
    c.register_singleton<ILogger, ConsoleLogger>();
    CHECK(c.has<ILogger>());
    c.clear();
    CHECK_FALSE(c.has<ILogger>());
}

// ── L-40: deterministic teardown ────────────────────────────────────────

TEST_CASE("Container: clear() destroys singletons in reverse registration order") {
    Recorder rec;
    {
        Container c;
        // Provider first, consumer second — the documented ordering rule.
        c.register_instance<IDatabase>(std::make_shared<IDatabase>(&rec, "database"));
        c.register_instance<ICache>(std::make_shared<ICache>(&rec, "cache"));
        c.clear();
        // Teardown already happened; the container is empty from here.
        CHECK_FALSE(c.has<IDatabase>());
        CHECK_FALSE(c.has<ICache>());
    }
    REQUIRE(rec.destroyed.size() == 2);
    CHECK(rec.destroyed[0] == "cache");      // consumer dies first
    CHECK(rec.destroyed[1] == "database");   // provider outlives it
}

TEST_CASE("Container: destructor honours the same reverse order as clear()") {
    Recorder rec;
    {
        Container c;
        c.register_instance<IDatabase>(std::make_shared<IDatabase>(&rec, "database"));
        c.register_instance<ICache>(std::make_shared<ICache>(&rec, "cache"));
        // No explicit clear() — ~Container must not fall back to the
        // unordered_map's unspecified destruction order.
    }
    REQUIRE(rec.destroyed.size() == 2);
    CHECK(rec.destroyed[0] == "cache");
    CHECK(rec.destroyed[1] == "database");
}

TEST_CASE("Container: re-registering a type keeps its teardown position") {
    Recorder rec;
    {
        Container c;
        c.register_instance<IDatabase>(std::make_shared<IDatabase>(&rec, "database-v1"));
        c.register_instance<ICache>(std::make_shared<ICache>(&rec, "cache"));
        // Replacing the provider must destroy the old instance right away
        // and must NOT move the type to the end of the teardown order.
        c.register_instance<IDatabase>(std::make_shared<IDatabase>(&rec, "database-v2"));
        REQUIRE(rec.destroyed.size() == 1);
        CHECK(rec.destroyed[0] == "database-v1");
        CHECK(c.resolve<IDatabase>()->name == "database-v2");
    }
    REQUIRE(rec.destroyed.size() == 3);
    CHECK(rec.destroyed[1] == "cache");
    CHECK(rec.destroyed[2] == "database-v2");
}

TEST_CASE("Container: a service destructor may re-enter the container") {
    bool saw_container = false;
    {
        Container c;
        c.register_singleton<ILogger, ConsoleLogger>();
        c.register_instance<SelfQuerying>(
            std::make_shared<SelfQuerying>(SelfQuerying{&c, &saw_container}));
        // ~SelfQuerying calls c.has<ILogger>(). It runs during clear(), so
        // the value must be destroyed with the container mutex released,
        // otherwise this line deadlocks instead of failing.
        c.clear();
    }
    // Registered after ILogger, so ILogger is still present when it dies.
    CHECK(saw_container);
}

TEST_CASE("Container: transient factories participate in teardown order") {
    Recorder rec;
    {
        Container c;
        c.register_instance<IDatabase>(std::make_shared<IDatabase>(&rec, "database"));
        // The factory's captured state is itself observable, so this pins
        // that factories share the one teardown order with singletons
        // rather than being cleared as a separate table.
        auto captured = std::make_shared<Tracked<3>>(&rec, "factory-capture");
        c.register_factory<ICache>([&rec, captured]() {
            return std::make_shared<ICache>(&rec, "cache-product");
        });
        captured.reset();  // the factory now owns the only reference

        c.clear();
        CHECK_FALSE(c.has<ICache>());
        CHECK_FALSE(c.has<IDatabase>());
    }
    // The factory was registered last, so its capture dies before the
    // provider it was allowed to depend on. "cache-product" never appears:
    // the factory was cleared without being invoked.
    REQUIRE(rec.destroyed.size() == 2);
    CHECK(rec.destroyed[0] == "factory-capture");
    CHECK(rec.destroyed[1] == "database");
}

TEST_CASE("Container: stays thread-safe while registering and resolving") {
    // Teardown moved value destruction outside the mutex; this keeps a
    // concurrent registrar/resolver pair on the shared path so the TSan
    // flavour has something to inspect beyond the single-threaded cases.
    Container c;
    c.register_singleton<ILogger, ConsoleLogger>();

    std::atomic<bool>        stop{false};
    std::atomic<int>         resolved{0};
    std::vector<std::thread> workers;

    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                if (auto p = c.resolve<ILogger>(); p) {
                    resolved.fetch_add(1, std::memory_order_relaxed);
                }
                (void)c.has<IDatabase>();
            }
        });
    }
    // Re-register the same type repeatedly: each replacement destroys the
    // previous instance while readers are resolving through the container.
    for (int i = 0; i < 200; ++i) {
        c.register_singleton<ILogger, ConsoleLogger>();
    }
    // Windows thread startup can outlast 200 cheap registrations: the
    // workers may observe stop == true on their first iteration and exit
    // without a single resolve. Wait (bounded) until they actually have.
    for (int i = 0; i < 2000 && resolved.load(std::memory_order_relaxed) == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : workers) t.join();

    CHECK(resolved.load() > 0);
    CHECK(c.resolve<ILogger>()->log("x") == "[console] x");
}

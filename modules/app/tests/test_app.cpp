#include <doctest/doctest.h>

#include "aria/app/app.hpp"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace aria::app;

struct Context { int creates{0}; };
struct Page : aria::binding::ViewModel {
    explicit Page(int value = 0) : id(value) {}
    int id;
};
struct OtherPage : aria::binding::ViewModel {};
struct ReentrantPage : Page {
    explicit ReentrantPage(std::function<void()> callback) : on_destroy(std::move(callback)) {}
    ~ReentrantPage() override { if (on_destroy) on_destroy(); }
    std::function<void()> on_destroy;
};
struct Destination {
    virtual ~Destination() = default;
    virtual bool on_navigate(const int& payload) = 0;
    virtual bool on_mount(const int& payload) = 0;
};
struct TargetPage : Page, Destination {
    bool accept{true};
    int received{-1};
    bool on_navigate(const int& payload) override { received = payload; return accept; }
    bool on_mount(const int& payload) override { received = payload; return accept; }
};
struct Module { std::string name; };
}  // namespace

TEST_CASE("application extensions reject duplicates and stale handles preserve replacements") {
    ExtensionRegistry<std::string, int> registry;
    int changes = 0;
    auto observer = registry.on_changed([&] { ++changes; });
    auto first = registry.add("slot", 1);
    CHECK(changes == 1);
    CHECK_THROWS_AS((void)registry.add("slot", 2), std::invalid_argument);
    CHECK(changes == 1);
    const auto original_generation = registry.generation("slot");
    CHECK(registry.remove("slot"));
    CHECK_FALSE(registry.remove("missing"));
    auto replacement = registry.add("slot", 3);
    CHECK(registry.generation("slot") > original_generation);
    first.release();
    CHECK(registry.find("slot") == 3);
    CHECK(changes == 3);
    CHECK(registry.set_enabled("slot", false));
    CHECK_FALSE(registry.contains("slot"));
    CHECK_FALSE(registry.find("slot"));
    CHECK(registry.generation("slot") == 0);
    CHECK(changes == 4);
    CHECK(registry.set_enabled("slot", false));
    CHECK(changes == 4);
    CHECK(registry.set_enabled("slot", true));
    CHECK(registry.find("slot") == 3);
    CHECK(changes == 5);
    CHECK_FALSE(registry.set_enabled("missing", false));
    replacement.release();
    CHECK_FALSE(registry.find("slot"));
    CHECK(changes == 6);
}

TEST_CASE("application extension snapshots are ordered copies and invocation can revoke itself") {
    ExtensionRegistry<std::string, std::function<int()>> registry;
    aria::Subscription first;
    aria::Subscription replacement;
    first = registry.add("first", [&] {
        first.release();
        replacement = registry.add("first", [] { return 99; });
        return 10;
    });
    auto second = registry.add("second", [] { return 20; });
    const auto snapshot = registry.snapshot();
    REQUIRE(snapshot.size() == 2);
    CHECK(snapshot[0].first == "first");
    CHECK(snapshot[1].first == "second");
    CHECK(snapshot[0].second() == 10);
    auto current = registry.find("first");
    REQUIRE(current);
    CHECK((*current)() == 99);
    const auto after = registry.snapshot();
    REQUIRE(after.size() == 2);
    CHECK(after[0].first == "second");
    CHECK(after[1].first == "first");
}

TEST_CASE("application extension notifications may destroy the registry and handles stay safe") {
    auto registry = std::make_unique<ExtensionRegistry<std::string, int>>();
    bool notified = false;
    auto observer = registry->on_changed([&] { notified = true; registry.reset(); });
    auto token = registry->add("slot", 1);
    CHECK(notified);
    CHECK_FALSE(registry);
    CHECK_NOTHROW(token.release());
    CHECK_NOTHROW(observer.release());
}

TEST_CASE("application clear revokes the old snapshot but preserves reentrant registration") {
    ExtensionRegistry<std::string, int> registry;
    auto first = registry.add("first", 1);
    auto second = registry.add("second", 2);
    aria::Subscription restored;
    auto observer = registry.on_changed([&] {
        if (registry.snapshot().empty()) restored = registry.add("first", 3);
    });
    registry.clear();
    first.release();
    second.release();
    CHECK(registry.find("first") == 3);
    CHECK(registry.snapshot().size() == 1);
    observer.release();
    restored.release();
    CHECK(registry.snapshot().empty());
}

TEST_CASE("application modules have stable order and retain no application virtual contract") {
    ModuleRegistry<Module> registry;
    auto alpha = std::make_shared<Module>("alpha");
    auto beta = std::make_shared<Module>("beta");
    auto early = std::make_shared<Module>("early");
    auto first = registry.add({"alpha", 10}, alpha);
    auto second = registry.add({"beta", 10}, beta);
    auto third = registry.add({"early", -5}, early);
    CHECK_THROWS_AS((void)registry.add({"alpha", 0}, beta), std::invalid_argument);
    CHECK_THROWS_AS((void)registry.add({"", 0}, beta), std::invalid_argument);
    CHECK_THROWS_AS((void)registry.add({"null", 0}, nullptr), std::invalid_argument);
    CHECK(registry.all() == std::vector<std::shared_ptr<Module>>{alpha, beta, early});
    CHECK(registry.ordered() == std::vector<std::shared_ptr<Module>>{early, alpha, beta});
    first.release();
    CHECK_FALSE(registry.find("alpha"));
    CHECK(registry.find("beta") == beta);
    registry.clear();
    CHECK(registry.all().empty());
}

TEST_CASE("application lifecycle rolls back failed startup and stops in reverse order") {
    std::vector<int> events;
    ModuleLifecycle lifecycle;
    const auto step = [&](int id) -> ModuleLifecycle::Start {
        return [&, id] {
            events.push_back(id);
            return aria::Subscription{[&, id] { events.push_back(-id); }};
        };
    };
    const std::vector<ModuleLifecycle::Start> failing{
        step(1), step(2), []() -> aria::Subscription { throw std::runtime_error("failed"); }};
    CHECK_THROWS_AS(lifecycle.start(failing), std::runtime_error);
    CHECK_FALSE(lifecycle.started());
    CHECK(events == std::vector<int>{1, 2, -2, -1});
    events.clear();
    const std::vector<ModuleLifecycle::Start> successful{step(1), step(2)};
    lifecycle.start(successful);
    lifecycle.start(successful);  // Idempotent while started.
    CHECK(lifecycle.started());
    CHECK(events == std::vector<int>{1, 2});
    lifecycle.stop();
    lifecycle.stop();
    CHECK_FALSE(lifecycle.started());
    CHECK(events == std::vector<int>{1, 2, -2, -1});
}

TEST_CASE("application lifecycle cancels reentrant stop and rejects recursive start") {
    int cleaned = 0;
    ModuleLifecycle lifecycle;
    const std::vector<ModuleLifecycle::Start> cancelled{[&] {
        lifecycle.stop();
        return aria::Subscription{[&] { ++cleaned; }};
    }};
    CHECK_THROWS_AS(lifecycle.start(cancelled), std::runtime_error);
    CHECK_FALSE(lifecycle.started());
    CHECK(cleaned == 1);
    const std::vector<ModuleLifecycle::Start> recursive{[&] {
        lifecycle.start({});
        return aria::Subscription{};
    }};
    CHECK_THROWS_AS(lifecycle.start(recursive), std::logic_error);
    CHECK_FALSE(lifecycle.started());
    lifecycle.start({});
    CHECK(lifecycle.started());
}

TEST_CASE("application lifecycle shutdown cannot silently restart from a cleanup callback") {
    ModuleLifecycle lifecycle;
    bool rejected = false;
    lifecycle.start({[&] {
        return aria::Subscription{[&] {
            lifecycle.stop();  // Reentrant stop is harmless.
            try {
                lifecycle.start({});
            } catch (const std::logic_error&) {
                rejected = true;
            }
        }};
    }});
    lifecycle.stop();
    CHECK(rejected);
    CHECK_FALSE(lifecycle.started());
    lifecycle.start({});
    CHECK(lifecycle.started());
}

TEST_CASE("application VM provider caches only primary instances and retries null or failing factories") {
    Context state;
    ViewModelProvider<Context> provider(state);
    auto factory = provider.add("page", [](Context& ctx) { return std::make_shared<Page>(++ctx.creates); });
    auto first = provider.get_as<Page>("page");
    REQUIRE(first);
    CHECK(first->id == 1);
    CHECK(provider.get("page") == first);
    auto fresh = provider.create("page");
    CHECK(fresh != first);
    CHECK(state.creates == 2);
    CHECK_FALSE(provider.get_as<OtherPage>("page"));
    CHECK_FALSE(provider.get("missing"));
    int attempts = 0;
    auto nullable = provider.add("nullable", [&](Context&) -> std::shared_ptr<aria::binding::ViewModel> {
        if (++attempts == 1) return nullptr;
        return std::make_shared<Page>();
    });
    CHECK_FALSE(provider.get("nullable"));
    CHECK(provider.get("nullable") != nullptr);
    CHECK(attempts == 2);
    attempts = 0;
    auto throwing = provider.add("throwing", [&](Context&) -> std::shared_ptr<aria::binding::ViewModel> {
        if (++attempts == 1) throw std::runtime_error("retry");
        return std::make_shared<Page>();
    });
    CHECK_THROWS_AS((void)provider.get("throwing"), std::runtime_error);
    CHECK(provider.get("throwing") != nullptr);
    provider.clear_cache();
    CHECK(provider.get("page") != first);
}

TEST_CASE("application VM provider invalidates only revoked factories and rejects recursion") {
    Context state;
    ViewModelProvider<Context> provider(state);
    auto a = provider.add("a", [](Context&) { return std::make_shared<Page>(); });
    auto b = provider.add("b", [](Context&) { return std::make_shared<Page>(); });
    std::weak_ptr<aria::binding::ViewModel> first = provider.get("a");
    auto untouched = provider.get("b");
    a.release();
    CHECK(first.expired());
    CHECK(provider.get("b") == untouched);
    auto c = provider.add("c", [](Context&) { return std::make_shared<Page>(); });
    CHECK(provider.get("b") == untouched);
    auto recursive = provider.add("recursive", [&](Context&) { return provider.get("recursive"); });
    CHECK_THROWS_AS((void)provider.get("recursive"), std::logic_error);
    CHECK_THROWS_AS((void)provider.get("recursive"), std::logic_error);
}

TEST_CASE("application VM factory may revoke and replace itself without caching the stale result") {
    Context state;
    ViewModelProvider<Context> provider(state);
    aria::Subscription original, replacement;
    original = provider.add("page", [&](Context&) {
        original.release();
        replacement = provider.add("page", [](Context&) { return std::make_shared<Page>(2); });
        return std::make_shared<Page>(1);
    });
    auto old = provider.get_as<Page>("page");
    REQUIRE(old);
    CHECK(old->id == 1);
    auto current = provider.get_as<Page>("page");
    REQUIRE(current);
    CHECK(current->id == 2);
    CHECK(provider.get("page") == current);
}

TEST_CASE("application VM invalidation and clear detach entries before reentrant destruction") {
    Context state;
    ViewModelProvider<Context> provider(state);
    int destruction_calls = 0;
    std::vector<aria::Subscription> registrations;
    std::vector<std::shared_ptr<aria::binding::ViewModel>> added;
    auto victim = provider.add("victim", [&](Context&) {
        return std::make_shared<ReentrantPage>([&] {
            ++destruction_calls;
            for (int index = 0; index < 20; ++index) {
                const auto key = "added-" + std::to_string(index);
                registrations.push_back(provider.add(key, [](Context&) { return std::make_shared<Page>(); }));
                added.push_back(provider.get(key));
            }
        });
    });
    (void)provider.get("victim");
    victim.release();
    CHECK(destruction_calls == 1);
    REQUIRE(added.size() == 20);
    for (int index = 0; index < 20; ++index) {
        CHECK(provider.get("added-" + std::to_string(index)) == added[static_cast<std::size_t>(index)]);
    }
    auto preserve = provider.add("preserve", [](Context&) { return std::make_shared<Page>(); });
    auto previous = provider.get("preserve");
    auto during_clear = provider.add("during-clear", [&](Context&) {
        return std::make_shared<ReentrantPage>([&] { added.push_back(provider.get("preserve")); });
    });
    (void)provider.get("during-clear");
    provider.clear_cache();
    REQUIRE(added.size() == 21);
    CHECK(added.back() != previous);
    CHECK(provider.get("preserve") == added.back());
}

TEST_CASE("application VM provider shuts down before destroying cached VMs and factory captures") {
    Context state;
    auto owner = std::make_unique<ViewModelProvider<Context>>(state);
    auto* provider = owner.get();
    int destruction_calls = 0;
    int factory_calls = 0;
    aria::Subscription spare;
    auto on_destroy = [&] {
        ++destruction_calls;
        CHECK_FALSE(provider->get("page"));
        CHECK_FALSE(provider->get("missing"));
        CHECK_FALSE(provider->get_as<Page>("page"));
        CHECK_FALSE(provider->create("page"));
        CHECK_THROWS_AS((void)provider->add("late", [](Context&) {
            return std::make_shared<Page>();
        }), std::logic_error);
        // Registration tokens and explicit cache cleanup remain safe during
        // shutdown; neither can restart work or visit partially dead members.
        spare.release();
        provider->clear_cache();
    };
    auto page = provider->add("page", [&](Context&) {
        ++factory_calls;
        return std::make_shared<ReentrantPage>(on_destroy);
    });
    spare = provider->add("spare", [](Context&) { return std::make_shared<Page>(); });
    auto factory_capture = std::make_shared<ReentrantPage>(on_destroy);
    auto captured = provider->add("captured", [factory_capture](Context&) {
        return factory_capture;
    });
    factory_capture.reset();
    (void)provider->get("page");
    owner.reset();
    CHECK(destruction_calls == 2);
    CHECK(factory_calls == 1);
    // Handles can also outlive the provider itself.
    CHECK_NOTHROW(page.release());
    CHECK_NOTHROW(captured.release());
}

TEST_CASE("application mounts reject mismatched interfaces and payloads and notify enable transitions") {
    Context state;
    MountRegistry<Context> mounts(state);
    auto wrong = mounts.provide("slot", "wrong", [](Context&) { return std::make_shared<OtherPage>(); });
    CHECK_FALSE(mounts.resolve<Destination>("slot", 1));
    CHECK_THROWS_AS((void)mounts.provide("slot", "duplicate", [](Context&) {
        return std::make_shared<Page>();
    }), std::invalid_argument);
    wrong.release();
    auto target = std::make_shared<TargetPage>();
    auto registration = mounts.provide("slot", "provider", [target](Context&) { return target; });
    target->accept = false;
    CHECK_FALSE(mounts.resolve<Destination>("slot", 2));
    CHECK(target->received == 2);
    target->accept = true;
    auto result = mounts.resolve<Destination>("slot", 3);
    REQUIRE(result);
    CHECK(result->vm == target);
    CHECK(result->provider_id == "provider");
    CHECK(target->received == 3);
    int changes = 0;
    auto observer = mounts.on_changed([&] { ++changes; });
    CHECK(mounts.set_enabled("slot", false));
    CHECK_FALSE(mounts.resolve("slot"));
    CHECK_FALSE(mounts.module_of("slot"));
    CHECK_FALSE(mounts.provided("slot"));
    CHECK(mounts.set_enabled("slot", false));
    CHECK(changes == 1);
    CHECK(mounts.set_enabled("slot", true));
    CHECK(mounts.module_of("slot") == "provider");
    CHECK(changes == 2);
}

TEST_CASE("application route and mount factories are copied before reentrant revocation") {
    Context state;
    MountRegistry<Context> mounts(state);
    aria::Subscription mount;
    mount = mounts.provide("slot", "once", [&](Context&) {
        mount.release();
        return std::make_shared<TargetPage>();
    });
    auto mounted = mounts.resolve<Destination>("slot", 1);
    REQUIRE(mounted);
    CHECK_FALSE(mounts.provided("slot"));
    CHECK(mounted->provider_id == "once");
    TypedRouteRegistry<Context> routes(state);
    aria::Subscription route;
    route = routes.add<Destination>("page", [&](Context&) {
        route.release();
        return std::make_shared<TargetPage>();
    });
    auto navigated = routes.resolve<Destination>(2);
    REQUIRE(navigated);
    CHECK(navigated->provider_id == "page");
    CHECK_FALSE(routes.resolve<Destination>(3));
    auto wrong = routes.add<Destination>("wrong", [](Context&) { return std::make_shared<OtherPage>(); });
    CHECK_FALSE(routes.resolve<Destination>(4));
    wrong.release();
    auto rejected = std::make_shared<TargetPage>();
    rejected->accept = false;
    auto rejects_payload = routes.add<Destination>("reject", [rejected](Context&) { return rejected; });
    CHECK_FALSE(routes.resolve<Destination>(5));
    CHECK(rejected->received == 5);
}

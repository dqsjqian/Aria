#include <doctest/doctest.h>

#include "aria/command.hpp"
#include "aria/property.hpp"

using namespace aria;

TEST_CASE("Command: basic execute") {
    int n = 0;
    Command<> cmd([&]() { ++n; });
    cmd.execute();
    CHECK(n == 1);
    cmd();
    CHECK(n == 2);
}

TEST_CASE("Command: can_execute predicate blocks invocation") {
    int n = 0;
    Property<bool> enabled(false);
    Command<> cmd(
        [&]() { ++n; },
        [&]() { return enabled.get(); }
    );
    cmd.execute();
    CHECK(n == 0);
    enabled = true;
    cmd.execute();
    CHECK(n == 1);
}

TEST_CASE("Command: observe_can_execute fires on notify") {
    Property<bool> enabled(true);
    Command<> cmd(
        []() {},
        [&]() { return enabled.get(); }
    );
    bool last = false;
    auto s = cmd.observe_can_execute([&](bool b) { last = b; });
    // Manual notify still works (escape hatch for non-reactive predicates).
    cmd.notify_can_execute_changed();
    CHECK(last == true);
    enabled = false;
    cmd.notify_can_execute_changed();
    CHECK(last == false);
}

TEST_CASE("Command<>: predicate auto-tracks reactive reads") {
    // When the predicate reads a Property, Command<> re-evaluates
    // automatically — no manual notify_can_execute_changed() required.
    Property<bool> enabled(false);
    Command<> cmd(
        []() {},
        [&]() { return enabled.get(); }
    );
    bool last = false;
    int  fires = 0;
    auto s = cmd.observe_can_execute([&](bool b) { last = b; ++fires; });

    // Subscriber attaches AFTER the Effect's eager first run, so the
    // initial value is not delivered (mirrors Property::on_changed).
    CHECK(cmd.can_execute() == false);
    CHECK(fires == 0);

    enabled = true;    // flips the truth value — should auto-emit once.
    CHECK(last == true);
    CHECK(fires == 1);

    enabled = true;    // no change — equality gate suppresses the emit.
    CHECK(fires == 1);

    enabled = false;   // flips back — one more emit.
    CHECK(last == false);
    CHECK(fires == 2);
}

TEST_CASE("Command<int>: parameterized command") {
    int captured = 0;
    Command<int> cmd([&](int x) { captured = x * 2; });
    cmd.execute(21);
    CHECK(captured == 42);
}

// ═══════════════════════════════════════════════════════════════════════
//  Command<>: eager auto-tracking contract
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("Command<>: observer attached later sees current state via can_execute()") {
    // The eager Effect deliberately suppresses its FIRST emit (the one
    // it does during construction) because no one can have observed us
    // yet. Callers that connect afterwards are expected to read
    // can_execute() synchronously to prime their UI, then rely on the
    // signal for future updates. Verify that contract.
    Property<bool> gate(true);
    Command<> cmd(
        []() {},
        [&]() { return gate.get(); }
    );

    // Current state is queryable synchronously.
    CHECK(cmd.can_execute());

    // Observer attached AFTER construction. No "priming" emit arrives.
    int emits = 0;
    bool last = false;
    auto sub = cmd.observe_can_execute([&](bool b) {
        ++emits;
        last = b;
    });
    CHECK(emits == 0);

    // Only the state flip fires a signal.
    gate = false;
    CHECK(emits == 1);
    CHECK_FALSE(last);
    CHECK_FALSE(cmd.can_execute());

    gate = true;
    CHECK(emits == 2);
    CHECK(last);
}

TEST_CASE("Command<>: equality gate — stable predicate outcome does not re-emit") {
    // The Effect re-runs whenever any upstream changes, but we only
    // want observers to see transitions in the can_execute truth value.
    // Changing an unrelated dep (or a dep whose value flips back before
    // the predicate would change answer) should be silent.
    Property<int> a(0), b(10);
    Command<> cmd(
        []() {},
        [&]() { return a.get() < b.get(); }    // true initially
    );

    int emits = 0;
    auto sub = cmd.observe_can_execute([&](bool) { ++emits; });

    // `a` moves but predicate still returns true → no emit.
    a = 1;
    a = 2;
    a = 3;
    CHECK(emits == 0);

    // Cross the boundary → emit once.
    a = 100;
    CHECK(emits == 1);
    CHECK_FALSE(cmd.can_execute());

    // Stay on the false side → silent.
    a = 200;
    CHECK(emits == 1);
}

TEST_CASE("Command<>: auto-tracks multiple independent dependencies") {
    Property<bool> flag_a(true);
    Property<bool> flag_b(true);
    Command<> cmd(
        []() {},
        [&]() { return flag_a.get() && flag_b.get(); }
    );

    int emits = 0;
    bool last = true;
    auto sub = cmd.observe_can_execute([&](bool b) { ++emits; last = b; });

    flag_a = false;
    CHECK(emits == 1);
    CHECK_FALSE(last);

    // Toggling `flag_b` while `flag_a` is false keeps the overall
    // answer false — NO emit.
    flag_b = false;
    CHECK(emits == 1);
    flag_b = true;
    CHECK(emits == 1);

    flag_a = true;  // now both true → true
    CHECK(emits == 2);
    CHECK(last);
}

TEST_CASE("Command<>: destroying the command disconnects its Effect cleanly") {
    // The Effect is owned by the Command; destroying the Command must
    // tear down the Effect and its reactive edges. Subsequent writes to
    // former upstreams must not dereference the dead signal.
    Property<bool> gate(true);

    int emits = 0;
    Subscription sub;
    {
        Command<> cmd(
            []() {},
            [&]() { return gate.get(); }
        );
        sub = cmd.observe_can_execute([&](bool) { ++emits; });

        gate = false;
        CHECK(emits == 1);
        // cmd goes out of scope here.
    }

    // The subscription's underlying signal is still alive (signal is
    // shared_ptr-backed), but there's no Effect pushing into it anymore.
    // Writing `gate` must NOT fire the observer.
    gate = true;
    CHECK(emits == 1);
    gate = false;
    CHECK(emits == 1);
}

TEST_CASE("Command<>: Property freed before Command -- destruction is still clean") {
    // Scenario: an upstream Property dies before the Command (atypical,
    // because normally the VM owns both and they die together, but we
    // want to be sure nothing in the reactive graph UAFs). The Source
    // node's destructor detaches every downstream edge in O(observers),
    // so after `gate.reset()` the Effect simply has no sources left.
    // It will NOT be re-triggered (no upstream to push it), and the
    // Command's destructor then tears down the Effect in the usual way.
    //
    // The key property under test: destroying the Command after its
    // only upstream is gone must not dereference freed storage, even
    // though the predicate lambda still captures a dangling reference.
    // We deliberately never call the predicate after `gate.reset()`.
    int emits = 0;
    auto gate = std::make_unique<Property<bool>>(true);
    auto cmd  = std::make_unique<Command<>>(
        []() {},
        [&g = *gate]() { return g.get(); }
    );
    auto sub = cmd->observe_can_execute([&](bool) { ++emits; });

    // Kill the upstream property first. The Effect's source edge is
    // detached by `~Node`; no re-fire is possible because nothing is
    // pushing into the graph.
    gate.reset();

    // Destroy the command — this must be clean (ASan / UBSan would
    // flame out on any dangling access).
    cmd.reset();
    CHECK(emits == 0);
}

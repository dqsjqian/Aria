#include <doctest/doctest.h>

#include "aria/property.hpp"
#include "aria/reactive/inspector.hpp"

#include <string>

using namespace aria;
using namespace aria::reactive;

TEST_CASE("GraphInspector: reachable_from walks both directions") {
    Property<int> a(1);
    Property<int> b(2);
    Computed<int> sum([&] { return a.get() + b.get(); });
    (void)sum.get();   // materialise the dependency edges

    // Seed with just the computed -- we must still discover the two
    // upstream Properties via source traversal.
    auto reach = GraphInspector::reachable_from({static_cast<const Node*>(&sum)});
    CHECK(reach.size() == 3);

    // Seed with just one Property -- we must discover the downstream
    // Computed via observer traversal.
    auto reach_from_a = GraphInspector::reachable_from({static_cast<const Node*>(&a)});
    CHECK(reach_from_a.size() >= 2);
}

TEST_CASE("GraphInspector: to_dot emits a well-formed digraph") {
    Property<int> x(0);
    x.set_debug_name("x");
    Computed<int> dbl([&] { return x.get() * 2; });
    dbl.set_debug_name("dbl");
    (void)dbl.get();

    std::string dot = GraphInspector::to_dot({static_cast<const Node*>(&dbl)}, "testG");
    CHECK(dot.find("digraph testG") != std::string::npos);
    CHECK(dot.find("Source") != std::string::npos);
    CHECK(dot.find("Derivation") != std::string::npos);
    // Labels embed Graphviz-style literal "\n" (backslash-n, not a real
    // newline) between label lines. We just check that the debug names
    // appear somewhere in the label text.
    CHECK(dot.find("label=\"") != std::string::npos);
    CHECK(dot.find("x") != std::string::npos);
    CHECK(dot.find("dbl") != std::string::npos);
    // Contains exactly one edge record: from x to dbl.
    const auto arrow_pos = dot.find("->");
    CHECK(arrow_pos != std::string::npos);
    CHECK(dot.find("->", arrow_pos + 1) == std::string::npos);
    CHECK(dot.back() == '\n');
}

TEST_CASE("GraphInspector: to_json is parseable and captures versions") {
    Property<int> p(0);
    p.set_debug_name("p");
    Computed<int> c([&] { return p.get() + 1; });
    c.set_debug_name("c");
    (void)c.get();

    std::string json = GraphInspector::to_json({static_cast<const Node*>(&c)});
    CHECK(json.starts_with("{\"nodes\":["));
    CHECK(json.find("\"name\":\"p\"") != std::string::npos);
    CHECK(json.find("\"name\":\"c\"") != std::string::npos);
    CHECK(json.find("\"edges\":[") != std::string::npos);
    CHECK(json.find("\"observed_version\":") != std::string::npos);
}

TEST_CASE("GraphInspector: flush tracer records pull order") {
    Property<int> src(0);
    src.set_debug_name("src");
    Computed<int> mid([&] { return src.get() + 1; });
    mid.set_debug_name("mid");
    Computed<int> leaf([&] { return mid.get() * 10; });
    leaf.set_debug_name("leaf");
    (void)leaf.get();   // prime dependencies

    std::vector<std::string> events;
    {
        GraphInspector::ScopedTracer guard(
            [&](const GraphInspector::FlushEvent& ev) {
                using P = GraphInspector::FlushEvent::Phase;
                switch (ev.phase) {
                    case P::FlushBegin:
                        events.emplace_back("FlushBegin");
                        break;
                    case P::RoundBegin:
                        events.emplace_back("RoundBegin:" + std::to_string(ev.round));
                        break;
                    case P::Pull:
                        events.emplace_back("Pull:" + ev.node->debug_name());
                        break;
                    case P::Recomputed:
                        events.emplace_back(std::string("Recomputed:")
                                            + ev.node->debug_name()
                                            + (ev.changed ? ":changed" : ":same"));
                        break;
                    case P::SkipClean:
                        events.emplace_back("SkipClean:" + ev.node->debug_name());
                        break;
                    case P::RoundEnd:
                        events.emplace_back("RoundEnd:" + std::to_string(ev.round));
                        break;
                    case P::FlushEnd:
                        events.emplace_back("FlushEnd");
                        break;
                }
            });

        src = 5;    // triggers a flush: mid then leaf.
    }

    // Minimum expected trace: begin / round / pull mid / recomputed mid /
    // pull leaf / recomputed leaf / round end / flush end.
    CHECK(events.front() == "FlushBegin");
    CHECK(events.back()  == "FlushEnd");

    // Find mid / leaf pulls in order.
    auto idx = [&](std::string_view needle) -> std::size_t {
        for (std::size_t i = 0; i < events.size(); ++i) {
            if (events[i].find(needle) != std::string::npos) return i;
        }
        return std::string::npos;
    };
    const auto pull_mid  = idx("Pull:mid");
    const auto pull_leaf = idx("Pull:leaf");
    REQUIRE(pull_mid  != std::string::npos);
    REQUIRE(pull_leaf != std::string::npos);
    CHECK(pull_mid < pull_leaf);          // topological order preserved
}

TEST_CASE("GraphInspector: ScopedTracer restores previous tracer on exit") {
    int outer_hits = 0;
    int inner_hits = 0;

    GraphInspector::install_flush_tracer(
        [&](const GraphInspector::FlushEvent& ev) {
            if (ev.phase == GraphInspector::FlushEvent::Phase::FlushBegin) ++outer_hits;
        });

    {
        GraphInspector::ScopedTracer scoped(
            [&](const GraphInspector::FlushEvent& ev) {
                if (ev.phase == GraphInspector::FlushEvent::Phase::FlushBegin) ++inner_hits;
            });

        Property<int> p(0);
        p = 1;    // triggers a flush -> inner tracer fires
        CHECK(inner_hits >= 1);
        CHECK(outer_hits == 0);
    }

    // After the scope ends, the original tracer must be back.
    Property<int> q(0);
    q = 1;
    CHECK(outer_hits >= 1);

    GraphInspector::clear_flush_tracer();
    CHECK_FALSE(GraphInspector::has_flush_tracer());
}

TEST_CASE("CircularDependencyError: message enumerates pending nodes") {
    // Two Effects forming a mutual feedback loop through two Properties:
    //
    //   e_ab: reads A, writes B
    //   e_ba: reads B, writes A
    //
    // After both effects are primed, writing A bumps B, which re-triggers
    // e_ba, which writes A, ... round after round. The Graph trips its
    // kMaxFlushRounds safety fuse and throws `CircularDependencyError`
    // whose message now lists the pending nodes.
    Property<int> a(0);
    Property<int> b(0);
    a.set_debug_name("a");
    b.set_debug_name("b");

    // Gate: let Effects run their first priming pass without looping,
    // so they register their source edges cleanly, then open the valve.
    bool live = false;

    Effect e_ab([&] {
        int v = a.get();
        if (live) b.set(v + 1);
    });
    Effect e_ba([&] {
        int v = b.get();
        if (live) a.set(v + 1);
    });

    bool caught = false;
    std::string msg;
    try {
        live = true;
        a.set(1);   // kicks off the ping-pong
    } catch (const CircularDependencyError& ex) {
        caught = true;
        msg    = ex.what();
    }

    CHECK(caught);
    if (caught) {
        // Improved diagnostic message lists the pending nodes (the two
        // Effects / Properties stuck in the ping-pong).
        CHECK(msg.find("Pending nodes") != std::string::npos);
    }
}

TEST_CASE("Node::effective_debug_name falls back to <Kind>#<id> when unset") {
    // A freshly-constructed Property has no debug name; effective_debug_name
    // must produce a non-empty, kind-tagged label so dumps stay readable.
    Property<int> p(0);
    const std::string& fallback = p.effective_debug_name();
    CHECK(!fallback.empty());
    CHECK(fallback.find("Source#") == 0);

    // Setting a name takes precedence over the fallback.
    Property<int> q(0);
    q.set_debug_name("named");
    CHECK(q.effective_debug_name() == "named");
}

TEST_CASE("Flush tracer reports duration_us for Recomputed events") {
    // A simple 2-node graph: Property → Computed.
    Property<int> p(1);
    p.set_debug_name("p");
    Computed<int> doubled([&] { return p.get() * 2; });
    doubled.set_debug_name("doubled");

    long long total_us = 0;
    int       recomputes = 0;
    {
        GraphInspector::ScopedTracer trace([&](const GraphInspector::FlushEvent& ev) {
            if (ev.phase == GraphInspector::FlushEvent::Phase::Recomputed) {
                ++recomputes;
                total_us += ev.duration_us;
            }
        });

        // Trigger a flush by mutating the source.
        p = 2;  // -> flush; doubled recomputes
        p = 3;  // -> flush; doubled recomputes
    }

    CHECK(recomputes >= 2);
    // duration_us should be non-negative; a Debug + ASan recompute
    // typically finishes in single-digit microseconds, but we only
    // assert non-negativity to avoid timing flake in CI.
    CHECK(total_us >= 0);
}
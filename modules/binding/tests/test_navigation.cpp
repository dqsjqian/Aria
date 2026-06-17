#include <doctest/doctest.h>

#include "aria/binding/navigation.hpp"

using namespace aria::binding;

namespace {

struct PageVm : ViewModel {
    int id = 0;
    int activated = 0;
    int deactivated = 0;
    explicit PageVm(int i) : id(i) {}
    void on_activate() override { ++activated; }
    void on_deactivate() override { ++deactivated; }
};

}  // namespace

TEST_CASE("Navigator: push activates new page and deactivates old current") {
    Navigator nav;
    auto a = nav.push<PageVm>(1);
    CHECK(nav.depth.get() == 1);
    CHECK(nav.current.get() == a);
    CHECK(a->activated == 1);
    CHECK(a->is_active().get());

    auto b = nav.push<PageVm>(2);
    CHECK(nav.depth.get() == 2);
    CHECK(nav.current.get() == b);
    CHECK_FALSE(a->is_active().get());
    CHECK(b->is_active().get());
    CHECK(a->deactivated == 1);
    CHECK(b->activated == 1);
}

TEST_CASE("Navigator: pop reactivates previous page") {
    Navigator nav;
    auto a = nav.push<PageVm>(1);
    auto b = nav.push<PageVm>(2);

    CHECK(nav.pop());
    CHECK(nav.depth.get() == 1);
    CHECK(nav.current.get() == a);
    CHECK(a->is_active().get());
    CHECK_FALSE(b->is_active().get());
    CHECK(a->activated == 2);  // first push + reactivation after pop
}

TEST_CASE("Navigator: replace swaps current page") {
    Navigator nav;
    auto a = nav.push<PageVm>(1);
    auto b = nav.replace<PageVm>(2);

    CHECK(nav.depth.get() == 1);
    CHECK(nav.current.get() == b);
    CHECK(a->deactivated == 1);
    CHECK_FALSE(a->is_active().get());
    CHECK(b->is_active().get());
}

TEST_CASE("Navigator: pop_to_root") {
    Navigator nav;
    auto root = nav.push<PageVm>(1);
    nav.push<PageVm>(2);
    nav.push<PageVm>(3);

    nav.pop_to_root();
    CHECK(nav.depth.get() == 1);
    CHECK(nav.current.get() == root);
    CHECK(root->is_active().get());
}

TEST_CASE("Navigator: clear deactivates everything") {
    Navigator nav;
    auto a = nav.push<PageVm>(1);
    auto b = nav.push<PageVm>(2);

    nav.clear();
    CHECK(nav.depth.get() == 0);
    CHECK(nav.current.get() == nullptr);
    CHECK_FALSE(a->is_active().get());
    CHECK_FALSE(b->is_active().get());
}

// =========================================================================
//  N-1 / N-2 / N-3 / N-4 protocol pinning
// =========================================================================

// ----------------------------------------------------------------------------
//  N-1: Presentation::Modal vs Push, dismiss_modal()
// ----------------------------------------------------------------------------
TEST_CASE("N-1: pushing a modal does not affect the previous Push entry's lifecycle") {
    Navigator nav;
    auto a = nav.push<PageVm>(1);
    auto b = std::make_shared<PageVm>(2);
    nav.push(b, Presentation::Modal);

    CHECK(nav.depth.get() == 2);
    CHECK(nav.current.get() == b);
    CHECK(nav.top_presentation() == Presentation::Modal);
    CHECK(b->is_active().get());
    // The Push entry below is deactivated (one entry "current" at a time).
    CHECK_FALSE(a->is_active().get());
}

TEST_CASE("N-1: dismiss_modal pops only when topmost is a modal") {
    Navigator nav;
    nav.push<PageVm>(1);

    // Topmost is Push -> dismiss_modal returns false.
    CHECK_FALSE(nav.dismiss_modal());
    CHECK(nav.depth.get() == 1);

    auto m = std::make_shared<PageVm>(99);
    nav.push(m, Presentation::Modal);
    CHECK(nav.dismiss_modal());
    CHECK(nav.depth.get() == 1);
}

// ----------------------------------------------------------------------------
//  N-2: push_for_result -- typed result delivery on dismiss
// ----------------------------------------------------------------------------
TEST_CASE("N-2: dismiss_with delivers the typed result to the awaiter") {
    Navigator nav;
    nav.push<PageVm>(1);                                 // parent

    auto fut = nav.push_for_result<int, PageVm>(42);
    CHECK(nav.depth.get() == 2);
    CHECK(fut.valid());

    CHECK(nav.dismiss_with<int>(7));
    REQUIRE(fut.wait_for(std::chrono::seconds(0))
            == std::future_status::ready);
    auto r = fut.get();
    REQUIRE(r.has_value());
    CHECK(*r == 7);
    CHECK(nav.depth.get() == 1);
}

TEST_CASE("N-2: pop without dismiss_with resolves the result to nullopt") {
    Navigator nav;
    nav.push<PageVm>(1);

    auto fut = nav.push_for_result<std::string, PageVm>(2);
    CHECK(nav.pop());                                    // no dismiss_with
    REQUIRE(fut.wait_for(std::chrono::seconds(0))
            == std::future_status::ready);
    auto r = fut.get();
    CHECK_FALSE(r.has_value());
}

TEST_CASE("N-2: navigator destruction resolves pending result to nullopt") {
    std::shared_future<std::optional<int>> fut;
    {
        Navigator nav;
        nav.push<PageVm>(1);
        fut = nav.push_for_result<int, PageVm>(2);
        // nav goes out of scope -> tear_down_top_(std::nullopt) per entry.
    }
    REQUIRE(fut.wait_for(std::chrono::seconds(0))
            == std::future_status::ready);
    CHECK_FALSE(fut.get().has_value());
}

TEST_CASE("N-2: type-mismatched dismiss_with falls back to nullopt") {
    Navigator nav;
    nav.push<PageVm>(1);
    auto fut = nav.push_for_result<int, PageVm>(2);

    // Caller asked for int, callee returns string -> bad_any_cast in
    // setter -> resolve nullopt rather than throw across the
    // pop_for_result API surface.
    CHECK(nav.dismiss_with<std::string>("oops"));
    REQUIRE(fut.wait_for(std::chrono::seconds(0))
            == std::future_status::ready);
    CHECK_FALSE(fut.get().has_value());
}

// ----------------------------------------------------------------------------
//  N-3: per-entry CancellationSource fires on pop
// ----------------------------------------------------------------------------
TEST_CASE("N-3: top_token is a fresh source per entry; pop fires it") {
    Navigator nav;
    nav.push<PageVm>(1);
    auto tok_a = nav.top_token();
    CHECK_FALSE(tok_a.is_cancelled());

    nav.push<PageVm>(2);
    auto tok_b = nav.top_token();
    CHECK_FALSE(tok_a.is_cancelled());
    CHECK_FALSE(tok_b.is_cancelled());

    CHECK(nav.pop());
    // Popping page 2 cancels its token; page 1's token is untouched.
    CHECK(tok_b.is_cancelled());
    CHECK_FALSE(tok_a.is_cancelled());

    CHECK(nav.pop());
    CHECK(tok_a.is_cancelled());
}

TEST_CASE("N-3: clear cancels every entry's token") {
    Navigator nav;
    nav.push<PageVm>(1);
    auto tok_a = nav.top_token();
    nav.push<PageVm>(2);
    auto tok_b = nav.top_token();

    nav.clear();
    CHECK(tok_a.is_cancelled());
    CHECK(tok_b.is_cancelled());
}

// ----------------------------------------------------------------------------
//  N-4: route() resolves a registered pattern + captures path params
// ----------------------------------------------------------------------------
TEST_CASE("N-4: register_route + route() pushes the matched factory") {
    Navigator nav;
    Navigator::RouteParams captured;
    nav.register_route("users/{id}",
        [&](const Navigator::RouteParams& p) -> std::shared_ptr<aria::binding::ViewModel> {
            captured = p;
            return std::make_shared<PageVm>(0);
        });

    CHECK(nav.route("users/42"));
    CHECK(nav.depth.get() == 1);
    CHECK(captured["id"] == "42");
}

TEST_CASE("N-4: route() returns false on no match (stack unchanged)") {
    Navigator nav;
    nav.push<PageVm>(1);
    nav.register_route("users/{id}",
        [](const Navigator::RouteParams&) -> std::shared_ptr<aria::binding::ViewModel> {
            return std::make_shared<PageVm>(0);
        });

    CHECK_FALSE(nav.route("posts/42"));
    CHECK(nav.depth.get() == 1);   // unchanged
}

TEST_CASE("N-4: route() with clear_stack=true replaces the entire stack") {
    Navigator nav;
    nav.push<PageVm>(1);
    nav.push<PageVm>(2);
    nav.register_route("home",
        [](const Navigator::RouteParams&) -> std::shared_ptr<aria::binding::ViewModel> {
            return std::make_shared<PageVm>(99);
        });

    RouteOptions opts;
    opts.clear_stack = true;
    CHECK(nav.route("home", opts));
    CHECK(nav.depth.get() == 1);
}

TEST_CASE("N-4: route() with Modal presentation marks the entry as modal") {
    Navigator nav;
    nav.push<PageVm>(1);
    nav.register_route("alert/{kind}",
        [](const Navigator::RouteParams&) -> std::shared_ptr<aria::binding::ViewModel> {
            return std::make_shared<PageVm>(0);
        });

    RouteOptions opts;
    opts.presentation = Presentation::Modal;
    CHECK(nav.route("alert/error", opts));
    CHECK(nav.top_presentation() == Presentation::Modal);
}

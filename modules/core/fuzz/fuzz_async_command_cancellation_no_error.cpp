// fuzz_async_command_cancellation_no_error — docs/reference/error-model.md §7:
//
//    "AsyncCommand cancellation never surfaces on the error face"
//
// E-20 clause 2 is a *negative* contract, which is exactly the kind that
// rots quietly: nothing fails when cancellation starts leaking into
// `last_error`, the UI just grows a spurious red banner every time the
// user navigates away. Clauses 1 and 2 also interact, and the
// interaction is where the real risk lives:
//
//   clause 1: every execute() start clears both error properties
//   clause 2: a cancelled body leaves both *untouched*
//
// Read together they say something stronger than either alone: a
// cancellation must not merely avoid *writing* an error, it must not
// resurrect the error a previous failure left behind either — because
// clause 1 already cleared it on the way in. So the only correct state
// after `fail -> cancel` is **clean**, not "still showing the old
// failure". A "don't touch on cancel" implementation that forgets to
// clear on entry passes a naive test and fails this one.
//
// Strategy: random walks over a 3-symbol alphabet (succeed / fail /
// cancel) against one long-lived command, with a reference model that
// tracks only what the contract permits. Both executors are inline, so
// each `execute()` runs the whole chain synchronously and the assertion
// sees a settled state.
//
// The cancellation is raised by the body itself (`throw
// OperationCancelled`) rather than via the command's token: E-20 clause
// 2 is about the *classification* of that exception on the observation
// surface, and raising it directly keeps the fuzzer focused on the
// classifier instead of on token plumbing (which L-36 already fuzzes in
// fuzz_cancellation_race.cpp).

#include <doctest/doctest.h>

#include <optional>
#include <string>

#include "aria/async/async_command.hpp"
#include "aria/async/cancellation.hpp"
#include "aria/async/executor.hpp"
#include "aria/async/task.hpp"
#include "aria/error.hpp"
#include "fuzz_support.hpp"

namespace {

using aria::ErrorKind;
using aria::async::AsyncCommand;
using aria::async::InlineExecutor;
using aria::async::OperationCancelled;
using aria::async::Task;
using aria::async::TimeoutError;

/// What the body should do on this invocation.
enum class Step : int {
    Succeed = 0,
    FailRuntime,
    FailTimeout,
    Cancel,
};

/// The only three shapes E-20 allows the error face to be in.
enum class Face {
    Clean,      ///< nullopt / ""
    Failure,    ///< AsyncFailure, message == thrown what()
    Timeout,    ///< Timeout kind
};

}  // namespace

TEST_CASE("fuzz: E-20 cancellation never surfaces on AsyncCommand's error face") {
    auto rng = aria::fuzz::Rng{aria::fuzz::seed(0xE2'00'CA'11)};
    const std::size_t iters = aria::fuzz::iters();

    InlineExecutor ui;
    InlineExecutor worker;

    // The body's behaviour is chosen per-invocation through this slot,
    // so one command instance serves the whole walk — which is the
    // point: state must not leak forward across invocations except in
    // the ways the contract allows.
    Step next = Step::Succeed;

    AsyncCommand<int, int> cmd{ui, worker, [&next](int x) -> Task<int> {
        switch (next) {
            case Step::Succeed:
                co_return x;
            case Step::FailRuntime:
                throw std::runtime_error("boom");
            case Step::FailTimeout:
                // `TimeoutError` takes no message — its what() is fixed
                // at "operation timed out", which E-20 clause 3 says is
                // what lands in `last_error->message`.
                throw TimeoutError{};
            case Step::Cancel:
                throw OperationCancelled{};
        }
        co_return x;
    }};

    Face expected = Face::Clean;
    std::string expected_message;

    for (std::size_t i = 0; i < iters; ++i) {
        next = static_cast<Step>(rng.u32(0, 3));

        switch (next) {
            case Step::Succeed:
                // Clause 1 cleared on entry, nothing wrote after.
                expected = Face::Clean;
                expected_message.clear();
                break;
            case Step::FailRuntime:
                expected = Face::Failure;
                expected_message = "boom";
                break;
            case Step::FailTimeout:
                expected = Face::Timeout;
                expected_message = "operation timed out";
                break;
            case Step::Cancel:
                // The crux: clause 1 clears on entry and clause 2 adds
                // nothing, so the face is CLEAN — not "whatever the
                // previous failure left".
                expected = Face::Clean;
                expected_message.clear();
                break;
        }

        cmd.execute(static_cast<int>(i));

        const auto err = cmd.last_error.get();
        const auto msg = cmd.last_error_message.get();

        switch (expected) {
            case Face::Clean:
                CHECK_FALSE(err.has_value());
                CHECK(msg.empty());
                break;
            case Face::Failure:
                REQUIRE(err.has_value());
                CHECK(err->kind == ErrorKind::AsyncFailure);
                CHECK(err->source == "AsyncCommand");
                CHECK(err->message == expected_message);
                break;
            case Face::Timeout:
                REQUIRE(err.has_value());
                CHECK(err->kind == ErrorKind::Timeout);
                CHECK(err->source == "AsyncCommand");
                CHECK(err->message == expected_message);
                break;
        }

        // Clause 5: the twin projections never drift apart.
        if (err.has_value()) {
            CHECK(msg == err->message);
        } else {
            CHECK(msg.empty());
        }

        // A cancellation is never classified as an error kind, on any
        // path. Restating it as a standalone assertion so a failure
        // report names the actual invariant.
        if (next == Step::Cancel) {
            CHECK_FALSE(err.has_value());
        }

        // Everything settled synchronously (both executors inline).
        CHECK_FALSE(cmd.is_executing.get());
    }
}

TEST_CASE("fuzz: E-20 cancel after failure clears, cancel after success stays clean") {
    // The two orderings that matter, pinned directly rather than left
    // to the random walk to eventually produce. `fail -> cancel` is the
    // regression that a "don't touch on cancel" implementation without
    // clause 1's entry-clear would fail.
    auto rng = aria::fuzz::Rng{aria::fuzz::seed(0xE2'0C'A0'01)};
    const std::size_t iters = std::min(aria::fuzz::iters(), std::size_t{20'000});

    InlineExecutor ui;
    InlineExecutor worker;

    for (std::size_t i = 0; i < iters; ++i) {
        Step next = Step::Succeed;
        AsyncCommand<int, int> cmd{ui, worker, [&next](int x) -> Task<int> {
            if (next == Step::Cancel) throw OperationCancelled{};
            if (next == Step::FailRuntime) throw std::runtime_error("prior failure");
            co_return x;
        }};

        const bool fail_first = rng.u32(0, 1) == 1;

        next = fail_first ? Step::FailRuntime : Step::Succeed;
        cmd.execute(1);

        if (fail_first) {
            REQUIRE(cmd.last_error.get().has_value());
            CHECK(cmd.last_error_message.get() == "prior failure");
        } else {
            CHECK_FALSE(cmd.last_error.get().has_value());
        }

        next = Step::Cancel;
        cmd.execute(2);

        // Either way: clean. The prior failure does NOT survive the
        // cancelled invocation.
        CHECK_FALSE(cmd.last_error.get().has_value());
        CHECK(cmd.last_error_message.get().empty());
    }
}

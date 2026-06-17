#pragma once

// AsyncResource<T> -- SWR / TanStack-Query style data caching for aria.
//
//   Task<UserProfile> fetch_profile(Api& api, int id) { co_return co_await api.fetch_user(id); }
//   AsyncResource<UserProfile> profile{
//       ui_executor, net_pool,
//       [&](int id) { return fetch_profile(api, id); }
//   };
//
//   // View binds the four observable fields:
//   auto sub_loading = profile.is_loading.bind([](bool b){ spinner.visible = b; });
//   auto sub_error   = profile.error.on_changed([](auto& e){ if (e) toast(e->message); });
//   auto sub_data    = profile.data.on_changed([](auto& opt){ if (opt) render(*opt); });
//
//   profile.fetch(42);            // first call -- kicks off the fetch
//   profile.fetch(42);            // de-duped -- same key, no extra request
//   profile.fetch(99);            // different key -- new fetch
//   profile.invalidate();         // mark dirty; next fetch() will refetch even with same key
//   profile.refresh();            // force refetch with current key
//
// Observable surface (per docs/error-model.md):
//
//   - is_loading          : Property<bool>
//   - error               : Property<std::optional<aria::Error>>   (nullopt when fine)
//   - error_message       : Property<std::string>                  ("" when fine)
//   - data                : Property<std::optional<T>>
//
// SWR niceties (unchanged):
//   - last successful result is kept in `data` while a refresh is in flight
//     (the "stale-while-revalidate" pattern), so the UI doesn't flash empty.
//   - in-flight requests with the same key are deduped (only one network hit).
//   - by default keys are compared via `==`; pass a custom `KeyEq` for
//     fancier semantics.

#include "aria/async/cancellation.hpp"
#include "aria/async/executor.hpp"
#include "aria/async/task.hpp"
#include "aria/async/timeout.hpp"  // TimeoutError detection in error mapping
#include "aria/diagnostics.hpp"
#include "aria/error.hpp"
#include "aria/loadable.hpp"
#include "aria/property.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace aria::async {

namespace detail {

template<typename T, typename Key>
struct AsyncResourceState {
    IExecutor* ui;
    IExecutor* worker;
    CancellationSource cancel;

    // Public observable state (lives here so coroutines can mutate it
    // even after the outer AsyncResource has been destroyed).
    Property<bool>                          is_loading{false};
    Property<std::optional<::aria::Error>>  error{std::nullopt};
    Property<std::string>                   error_message{""};
    Property<std::optional<T>>              data{std::optional<T>{}};
    // Five-state loadable view-model. Updated whenever any of
    // (is_loading / data / error) is written. See `loadable.hpp`
    // for the LO-N protocol. Always kept consistent with the
    // four primitive Properties above; observers may bind to either
    // surface depending on whether they want raw fields or the
    // collapsed sum-type.
    Property<::aria::Loadable<T>>           loadable{::aria::Loadable<T>::idle()};

    // Book-keeping
    Key  current_key{};
    bool has_key{false};
    std::atomic<bool>          dirty{false};
    std::atomic<bool>          in_flight{false};
    std::atomic<std::uint64_t> gen{0};

    AsyncResourceState(IExecutor& u, IExecutor& w) : ui(&u), worker(&w) {}

    /// Re-derive `loadable` from (is_loading, data, error). Called
    /// after every state mutation in `AsyncResource` so the
    /// `Property<Loadable<T>>` surface stays consistent with the
    /// four primitive Properties. Equality-gated `set` (E-11 / L-21)
    /// keeps redundant writes from notifying observers.
    void recompute_loadable_() {
        const bool                          loading_now = is_loading.peek();
        const std::optional<T>              d           = data.peek();
        const std::optional<::aria::Error>  e           = error.peek();

        if (e.has_value()) {
            // Error state -- preserve last-good value (SWR) if any.
            if (d.has_value()) {
                loadable.set(::aria::Loadable<T>::error(*e, *d));
            } else {
                loadable.set(::aria::Loadable<T>::error(*e));
            }
            return;
        }
        if (loading_now) {
            if (d.has_value()) {
                loadable.set(::aria::Loadable<T>::refreshing(*d));
            } else {
                loadable.set(::aria::Loadable<T>::loading());
            }
            return;
        }
        if (d.has_value()) {
            loadable.set(::aria::Loadable<T>::success(*d));
            return;
        }
        loadable.set(::aria::Loadable<T>::idle());
    }
};

}  // namespace detail

template<typename T, typename Key = int>
class AsyncResource {
    using State = detail::AsyncResourceState<T, Key>;
    std::shared_ptr<State> state_;

public:
    using Fetcher = std::function<Task<T>(Key)>;

private:
    Fetcher fetcher_;

public:
    AsyncResource(IExecutor& ui, IExecutor& worker, Fetcher fetcher)
        : state_(std::make_shared<detail::AsyncResourceState<T, Key>>(ui, worker)),
          fetcher_(std::move(fetcher)),
          is_loading    (state_->is_loading),
          error         (state_->error),
          error_message (state_->error_message),
          data          (state_->data),
          loadable      (state_->loadable) {}

    ~AsyncResource() { state_->cancel.cancel(); }

    AsyncResource(const AsyncResource&) = delete;
    AsyncResource& operator=(const AsyncResource&) = delete;

    /// Fetch for `key`. If the same `key` already has fresh data and
    /// no `invalidate()` has been called since, this is a no-op. If a
    /// fetch for the same key is already in-flight, it's deduped.
    void fetch(Key key) {
        bool same_key = state_->has_key
                     && state_->current_key == key;

        if (same_key && !state_->dirty.load(std::memory_order_acquire)
                     && !is_loading.get()
                     && data.get().has_value()) {
            if (::aria::has_trace_sink()) {
                ::aria::publish_trace_unchecked(::aria::TraceCategory::Async,
                    ::aria::trace::Async{"AsyncResource", "cache_hit",
                                         state_->gen.load(std::memory_order_relaxed)});
            }
            return;  // cache hit
        }
        if (same_key && state_->in_flight.load(std::memory_order_acquire)) {
            if (::aria::has_trace_sink()) {
                ::aria::publish_trace_unchecked(::aria::TraceCategory::Async,
                    ::aria::trace::Async{"AsyncResource", "dedupe",
                                         state_->gen.load(std::memory_order_relaxed)});
            }
            return;  // de-dupe in-flight
        }

        state_->current_key = key;
        state_->has_key = true;
        do_fetch_(key);
    }

    void refresh() {
        if (!state_->has_key) return;
        invalidate();
        do_fetch_(state_->current_key);
    }

    void invalidate() noexcept {
        state_->dirty.store(true, std::memory_order_release);
    }

    void clear() {
        state_->has_key = false;
        state_->dirty.store(false, std::memory_order_release);
        state_->is_loading    = false;
        state_->error         = std::nullopt;
        state_->error_message = "";
        state_->data          = std::optional<T>{};
        state_->recompute_loadable_();
    }

    /// Cancel any in-flight fetch and drop its pending write-back, WITHOUT
    /// destroying the resource. This is the third lifetime axis from the
    /// cancellation model (see `docs/reference/lifecycle.md` L-38 and
    /// ROADMAP P1-H): wire it to a view-destroy hook so navigating away
    /// mid-request stops the coroutine from resuming against a dead view.
    ///
    ///     AsyncResource<UserProfile> profile{ui, net, fetch};
    ///     engine.bind_view_lifetime(view, [&profile]{ profile.cancel(); });
    ///
    /// Stale-while-revalidate is preserved: the last successful `data` is
    /// kept (only the loading flag is cleared), so a re-mounted view still
    /// renders the previous value while a fresh `fetch()`/`refresh()` runs.
    /// The resource remains fully usable afterwards — a cancelled
    /// `CancellationSource` stays cancelled forever, so we re-arm a fresh
    /// one here.
    ///
    /// Thread: must be called on the graph thread (it writes the observable
    /// Properties), exactly like `clear()` / `fetch()`.
    void cancel() {
        // 1. Flip the cooperative-cancel tokens every in-flight coroutine
        //    is holding; they unwind at their next `throw_if_cancelled`
        //    probe (after the ui/worker hops). The detached-task path
        //    swallows the resulting OperationCancelled.
        state_->cancel.cancel();
        // 2. Re-arm with a fresh source so future fetches are cancellable
        //    again (move-assign drops our handle to the now-cancelled state;
        //    in-flight coroutines keep their own token alive via shared_ptr).
        state_->cancel = CancellationSource{};
        // 3. Bump the generation so any run that lands after this point is
        //    dropped by the stale-gen guard in `run_one_`, and release the
        //    in-flight flag (R-1: we are now the flag's owner).
        state_->gen.fetch_add(1, std::memory_order_acq_rel);
        state_->in_flight.store(false, std::memory_order_release);
        // 4. Surface a non-loading state (SWR: last `data` is kept).
        state_->is_loading = false;
        state_->recompute_loadable_();
        if (::aria::has_trace_sink()) {
            ::aria::publish_trace_unchecked(::aria::TraceCategory::Async,
                ::aria::trace::Async{"AsyncResource", "cancel",
                                     state_->gen.load(std::memory_order_relaxed)});
        }
    }

    [[nodiscard]] bool has_data() const { return data.get().has_value(); }

    // ── Public observable handles ────────────────────────────────────
    //
    // `in_flight` invariant (R-1): scoped to the **latest** fetch
    // generation only. When a newer `fetch(...)` bumps `gen`, any
    // earlier in-flight run that lands afterwards is a *stale* run --
    // it observes `gen != my_gen`, drops its result, and MUST NOT
    // clear `in_flight`. The newer run is now the sole owner of the
    // flag and clears it on its own completion path. This guarantees
    // observers see `in_flight = true` continuously across rapid
    // key changes, not a brief false flicker caused by a stale
    // run's clean-up.
    Property<bool>&                         is_loading;
    Property<std::optional<::aria::Error>>& error;
    Property<std::string>&                  error_message;
    Property<std::optional<T>>&             data;

    /// Five-state loadable view-model -- `Idle / Loading /
    /// Refreshing / Success / Error`. Always consistent with the four
    /// primitive Properties above. UI code typically prefers this
    /// surface (one bind, one switch) over the four-handle surface.
    Property<::aria::Loadable<T>>&          loadable;

private:
    void do_fetch_(Key key) {
        state_->in_flight.store(true, std::memory_order_release);
        state_->dirty.store(false, std::memory_order_release);
        auto my_gen = state_->gen.fetch_add(1, std::memory_order_acq_rel) + 1;
        // Synchronously surface the in-flight state on the public
        // Properties so observers see Loading / Refreshing the moment
        // `fetch()` returns -- not after the worker hop. The same
        // assignments happen again inside `run_one_` after the UI
        // hop; equality-gated `set()` (E-11 / L-21) drops the second
        // write as a no-op.
        state_->is_loading    = true;
        state_->error         = std::nullopt;
        state_->error_message = "";
        state_->recompute_loadable_();
        if (::aria::has_trace_sink()) {
            ::aria::publish_trace_unchecked(::aria::TraceCategory::Async,
                ::aria::trace::Async{"AsyncResource", "fetch_start", my_gen});
        }
        auto runner = run_one_(key, my_gen);
        std::move(runner).start_detached();
    }

    Task<void> run_one_(Key key, std::uint64_t my_gen) {
        auto state   = state_;
        auto fetcher = fetcher_;
        auto tok     = state->cancel.token();

        co_await schedule_on(*state->ui);
        tok.throw_if_cancelled();
        state->is_loading    = true;
        state->error         = std::nullopt;
        state->error_message = "";
        state->recompute_loadable_();

        std::exception_ptr ex;
        std::optional<T> result;
        try {
            co_await schedule_on(*state->worker);
            tok.throw_if_cancelled();
            T v = co_await fetcher(key);
            result.emplace(std::move(v));
        } catch (...) {
            ex = std::current_exception();
        }

        co_await schedule_on(*state->ui);

        // Stale-result guard: only the latest fetch wins. Per the
        // resource's `in_flight` invariant (R-1, see
        // `docs/error-model.md` companion notes / and below): a stale
        // run MUST NOT touch `in_flight` -- the newer run that
        // bumped `gen` has already taken ownership of the flag. The
        // newer run's own completion path will clear it once it is
        // truly the last one in flight.
        if (state->gen.load(std::memory_order_acquire) != my_gen) {
            if (::aria::has_trace_sink()) {
                ::aria::publish_trace_unchecked(::aria::TraceCategory::Async,
                    ::aria::trace::Async{"AsyncResource", "stale_drop", my_gen});
            }
            co_return;
        }

        if (ex) {
            try { std::rethrow_exception(ex); }
            catch (const OperationCancelled&) {
                // Silent: cancellation is not surfaced as an
                // observable error; keep `error` as nullopt.
                if (::aria::has_trace_sink()) {
                    auto err = ::aria::Error::cancellation("AsyncResource");
                    ::aria::publish_trace_unchecked(::aria::TraceCategory::Async,
                        ::aria::trace::Async{"AsyncResource", "cancelled", my_gen},
                        std::move(err));
                }
            }
            catch (const TimeoutError& e) {
                auto err = ::aria::Error::timeout("AsyncResource");
                err.message            = e.what();
                if (::aria::has_trace_sink()) {
                    ::aria::publish_trace_unchecked(::aria::TraceCategory::Async,
                        ::aria::trace::Async{"AsyncResource", "timeout", my_gen},
                        err);
                }
                state->error_message   = err.message;
                state->error           = std::move(err);
            }
            catch (...) {
                auto err = ::aria::Error::from_exception(ex, "AsyncResource");
                if (::aria::has_trace_sink()) {
                    ::aria::publish_trace_unchecked(::aria::TraceCategory::Async,
                        ::aria::trace::Async{"AsyncResource", "failure", my_gen},
                        err);
                }
                state->error_message   = err.message;
                state->error           = std::move(err);
            }
        } else {
            state->data = result;
            if (::aria::has_trace_sink()) {
                ::aria::publish_trace_unchecked(::aria::TraceCategory::Async,
                    ::aria::trace::Async{"AsyncResource", "fetch_finish", my_gen});
            }
        }
        state->is_loading = false;
        state->in_flight.store(false, std::memory_order_release);
        state->recompute_loadable_();
    }
};

}  // namespace aria::async

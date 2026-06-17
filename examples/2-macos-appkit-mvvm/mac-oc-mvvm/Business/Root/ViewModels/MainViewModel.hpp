#pragma once

/// MainViewModel — C++ ViewModel using aria framework.
///
/// Replaces the original OC MainViewModel which used 5 separate
/// communication patterns (Block, NSNotification, KVO, Delegate, Target-Action).
///
/// With aria, everything is unified:
///   - Property<string>  replaces KVO / Block / Notification / Delegate callbacks
///   - Command<>         replaces Target-Action
///   - Property::bind()  replaces manual observer wiring
///
/// The View just binds to properties and invokes commands. One clean pattern.

#include "aria/aria.hpp"
#include "aria/binding/view_model.hpp"
#include "DataModel.h"

#include <functional>
#include <memory>
#include <string>

class MainViewModel : public aria::binding::ViewModel {
public:
    // ── Observable state (replaces all OC patterns) ─────────────────────────
    aria::Property<std::string> result_text{""};
    aria::Property<std::string> status_text{"Tap a button"};
    aria::Property<bool>        is_loading{false};

    // ── Command: unified fetch action ───────────────────────────────────────
    // All 5 OC buttons now funnel into this single command.
    aria::Command<std::string> fetch_cmd;

    // ── Main-thread dispatcher ──────────────────────────────────────────────
    // The View sets this to route callbacks to the main thread.
    // Signature: void(std::function<void()> work)
    std::function<void(std::function<void()>)> main_dispatcher;

    explicit MainViewModel(std::shared_ptr<DataModel> model)
        : model_(std::move(model)),
          fetch_cmd([this](const std::string& tag) { doFetch(tag); })
    {}

    ~MainViewModel() override = default;

    // Convenience for View: get a weak_ptr (for safe capture in async callbacks)
    std::weak_ptr<MainViewModel> weak_self() {
        return std::static_pointer_cast<MainViewModel>(shared_from_this());
    }

    // Factory: creates a shared_ptr
    static std::shared_ptr<MainViewModel> create(std::shared_ptr<DataModel> model) {
        // Can't use make_shared because constructor is public but base class
        // uses enable_shared_from_this. Use shared_ptr directly.
        return std::shared_ptr<MainViewModel>(new MainViewModel(std::move(model)));
    }

private:
    std::shared_ptr<DataModel> model_;

    void doFetch(const std::string& tag) {
        is_loading.set(true);
        status_text.set("Fetching: " + tag + "...");

        // Capture weak_ptr for safe async callback
        auto ws = weak_self();

        model_->fetchData(tag, [ws, tag](FetchResult result) {
            auto self = ws.lock();
            if (!self) return;  // ViewModel destroyed

            auto apply = [self, result, tag]() {
                self->is_loading.set(false);
                if (result.ok) {
                    self->result_text.set(result.message);
                    self->status_text.set("[" + tag + "] Done");
                } else {
                    self->result_text.set(result.message);
                    self->status_text.set("[" + tag + "] Error");
                }
            };

            // Dispatch to main thread if available
            if (self->main_dispatcher) {
                self->main_dispatcher(std::move(apply));
            } else {
                apply();  // Fallback: apply on current thread
            }
        });
    }
};

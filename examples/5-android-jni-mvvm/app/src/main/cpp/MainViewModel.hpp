#pragma once

/// MainViewModel — C++ ViewModel using aria framework.
///
/// Replaces the original Kotlin MainViewModel which used Android's
/// ViewModel + MutableStateFlow. Now the real MVVM logic lives in C++
/// with aria's Property / Command / ViewModel base class.
///
/// With aria, all 5 OC patterns are unified:
///   - Property<string>  replaces KVO / Block / Notification / Delegate callbacks
///   - Command<string>   replaces Target-Action
///
/// The Kotlin side only observes Property changes via JNI and updates
/// its StateFlow for Compose recomposition — a thin shell.

#include "aria/aria.hpp"
#include "aria/binding/view_model.hpp"
#include "DataModel.h"

#include <functional>
#include <memory>
#include <string>

class MainViewModel : public aria::binding::ViewModel {
public:
    // ── Observable state (replaces all OC patterns) ─────────────────────────
    aria::Property<std::string> status_text{"Tap a button"};
    aria::Property<std::string> result_text{""};
    aria::Property<bool>        is_loading{false};

    // ── Toast property ──────────────────────────────────────────────────────
    aria::Property<std::string> toast_message{""};

    // ── Commands: one per pattern button ─────────────────────────────────────
    // Each command takes a tag string identifying the pattern.
    // The JNI bridge calls execute("Block"), execute("Notification"), etc.
    aria::Command<std::string> fetch_cmd;

    // ── Dismiss toast command ─────────────────────────────────────────────
    aria::Command<> dismiss_toast;

    // ── Main-thread dispatcher ──────────────────────────────────────────────
    // The JNI bridge sets this to route callbacks to the JVM main thread.
    // Signature: void(std::function<void()> work)
    std::function<void(std::function<void()>)> main_dispatcher;

    explicit MainViewModel(std::shared_ptr<DataModel> model);
    ~MainViewModel() override = default;

    // Factory: creates a shared_ptr
    static std::shared_ptr<MainViewModel> create(std::shared_ptr<DataModel> model) {
        return std::shared_ptr<MainViewModel>(new MainViewModel(std::move(model)));
    }

    // Convenience: get a weak_ptr (for safe capture in async callbacks)
    std::weak_ptr<MainViewModel> weak_self() {
        return std::static_pointer_cast<MainViewModel>(shared_from_this());
    }

private:
    std::shared_ptr<DataModel> model_;

    void doFetch(const std::string& tag);
};

#pragma once

// QtDispatcher — bridge Qt's main event loop to aria::runtime::IDispatcher.
//
//   QApplication app(argc, argv);
//   auto dispatcher = std::make_shared<QtDispatcher>(&app);
//   runtime::set_main_dispatcher(dispatcher);
//
// Coroutines can then jump back to UI with:
//   co_await schedule_on(DispatcherExec{*dispatcher});
// or platform adapters can use dispatcher.post(...).

#include "aria/runtime/dispatcher.hpp"

#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
#include <QThread>
#include <QTimer>

#include <chrono>
#include <functional>
#include <utility>

namespace aria::adapters::qt6 {

class QtDispatcher final : public runtime::IDispatcher {
public:
    /// `context` must live at least as long as this dispatcher.  QApplication
    /// / QCoreApplication is a good default.
    explicit QtDispatcher(QObject* context = QCoreApplication::instance())
        : context_(context) {}

    void post(std::function<void()> fn) override {
        if (!context_) return;
        QMetaObject::invokeMethod(
            context_,
            [fn = std::move(fn)]() mutable { fn(); },
            Qt::QueuedConnection);
    }

    void post_delayed(std::chrono::milliseconds delay,
                      std::function<void()> fn) override {
        if (!context_) return;
        QTimer::singleShot(
            static_cast<int>(delay.count()),
            context_,
            [fn = std::move(fn)]() mutable { fn(); });
    }

    [[nodiscard]] bool is_main_thread() const noexcept override {
        return context_ && QThread::currentThread() == context_->thread();
    }

    /// Qt drives its own event loop, so this dispatcher is `Autonomous`
    /// (no application-level pumping) rather than `Pumpable`. Capability
    /// is overridden to reflect that — `IDispatcher`'s base default
    /// claims `Pumpable`, which is correct for `SimpleDispatcher` but
    /// not for a Qt-backed dispatcher.
    [[nodiscard]] ::aria::SchedulerCaps caps() const noexcept override {
        return ::aria::SchedulerCaps::Post
             | ::aria::SchedulerCaps::Delay
             | ::aria::SchedulerCaps::MainThread
             | ::aria::SchedulerCaps::Autonomous;
    }

private:
    QObject* context_ = nullptr;
};

}  // namespace aria::adapters::qt6

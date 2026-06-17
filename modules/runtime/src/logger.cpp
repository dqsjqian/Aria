#include "aria/runtime/logger.hpp"
#include <memory>
#include <atomic>
#include <cstdio>
#include <exception>
#include <mutex>

namespace aria::runtime {

const char* level_name(LogLevel l) noexcept {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "?????";
}

struct Logger::Impl {
    Logger::Sink sink;
    std::atomic<LogLevel> level{LogLevel::Info};
    std::mutex mutex;
};

Logger::Logger() : impl_(std::make_unique<Impl>()) {}
Logger::~Logger() = default;

Logger& Logger::instance() noexcept {
    static Logger inst;
    return inst;
}

void Logger::set_sink(Sink sink) {
    std::lock_guard lk(impl_->mutex);
    impl_->sink = std::move(sink);
}

void Logger::set_level(LogLevel level) noexcept { impl_->level.store(level, std::memory_order_relaxed); }
LogLevel Logger::level() const noexcept { return impl_->level.load(std::memory_order_relaxed); }

void Logger::log(LogLevel level, std::string_view category, std::string_view message) {
    if (static_cast<int>(level) < static_cast<int>(impl_->level.load(std::memory_order_relaxed))) return;

    Sink local_sink;
    {
        std::lock_guard lk(impl_->mutex);
        local_sink = impl_->sink;
    }

    if (local_sink) {
        // Contract: Logger never propagates exceptions out of log(). If a
        // user-installed sink throws, fall back to stderr with a marker so
        // the failure stays observable but never bubbles up to framework-
        // internal noexcept boundaries (executor workers, dispatcher pumps
        // etc.). This keeps the logger usable as a leaf reporter from any
        // call site.
        try {
            local_sink(level, category, message);
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                         "[%s][%.*s] %.*s  (sink threw: %s)\n",
                         level_name(level),
                         static_cast<int>(category.size()), category.data(),
                         static_cast<int>(message.size()), message.data(),
                         e.what());
        } catch (...) {
            std::fprintf(stderr,
                         "[%s][%.*s] %.*s  (sink threw: non-std exception)\n",
                         level_name(level),
                         static_cast<int>(category.size()), category.data(),
                         static_cast<int>(message.size()), message.data());
        }
    } else {
        std::fprintf(stderr, "[%s][%.*s] %.*s\n",
                     level_name(level),
                     static_cast<int>(category.size()), category.data(),
                     static_cast<int>(message.size()), message.data());
    }
}

}  // namespace aria::runtime

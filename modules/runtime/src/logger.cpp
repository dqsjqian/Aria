#include "aria/runtime/logger.hpp"
#include <memory>
#include <algorithm>
#include <limits>
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
    std::shared_ptr<const Logger::Sink> sink;
    bool closing = false;
    std::atomic<LogLevel> level{LogLevel::Info};
    std::mutex mutex;
};

Logger::Logger() : impl_(std::make_unique<Impl>()) {}
Logger::~Logger() {
    std::shared_ptr<const Sink> sink;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->closing = true;
        sink.swap(impl_->sink);
    }
}

Logger& Logger::instance() noexcept {
    static Logger inst;
    return inst;
}

void Logger::set_sink(Sink sink) {
    std::shared_ptr<const Sink> replacement;
    if (sink) replacement = std::make_shared<const Sink>(std::move(sink));
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->closing) impl_->sink.swap(replacement);
    }
}

void Logger::set_level(LogLevel level) noexcept { impl_->level.store(level, std::memory_order_relaxed); }
LogLevel Logger::level() const noexcept { return impl_->level.load(std::memory_order_relaxed); }

void Logger::log(LogLevel level, std::string_view category, std::string_view message) noexcept {
    if (static_cast<int>(level) < static_cast<int>(impl_->level.load(std::memory_order_relaxed))) return;

    static thread_local bool reporting = false;
    if (reporting) {
        std::fprintf(stderr, "[aria.logger] recursive log suppressed\n");
        return;
    }
    struct ReportGuard {
        bool& active;
        explicit ReportGuard(bool& value) : active(value) { active = true; }
        ~ReportGuard() { active = false; }
    } guard{reporting};
    std::shared_ptr<const Sink> local_sink;
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
            (*local_sink)(level, category, message);
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                         "[%s][%.*s] %.*s  (sink threw: %s)\n",
                         level_name(level),
                         static_cast<int>(std::min(category.size(), std::size_t{std::numeric_limits<int>::max()})),
                         category.empty() ? "" : category.data(),
                         static_cast<int>(std::min(message.size(), std::size_t{std::numeric_limits<int>::max()})),
                         message.empty() ? "" : message.data(),
                         e.what());
        } catch (...) {
            std::fprintf(stderr,
                         "[%s][%.*s] %.*s  (sink threw: non-std exception)\n",
                         level_name(level),
                         static_cast<int>(std::min(category.size(), std::size_t{std::numeric_limits<int>::max()})),
                         category.empty() ? "" : category.data(),
                         static_cast<int>(std::min(message.size(), std::size_t{std::numeric_limits<int>::max()})),
                         message.empty() ? "" : message.data());
        }
    } else {
        std::fprintf(stderr, "[%s][%.*s] %.*s\n",
                     level_name(level),
                     static_cast<int>(std::min(category.size(), std::size_t{std::numeric_limits<int>::max()})),
                         category.empty() ? "" : category.data(),
                     static_cast<int>(std::min(message.size(), std::size_t{std::numeric_limits<int>::max()})),
                         message.empty() ? "" : message.data());
    }
}

}  // namespace aria::runtime

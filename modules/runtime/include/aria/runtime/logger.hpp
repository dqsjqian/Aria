#pragma once

#include "aria/abi/export.hpp"
#include <functional>
#include <memory>
#include <string_view>

namespace aria::runtime {

enum class LogLevel { Trace, Debug, Info, Warn, Error, Fatal };

/// Process-wide logger. Implementation lives in the runtime shared library, so
/// there is exactly one global sink across all modules.
class ARIA_RUNTIME_API Logger {
public:
    using Sink = std::function<void(LogLevel, std::string_view, std::string_view /*message*/)>;

    /// Get the singleton (resolved from the runtime shared library, never duplicated).
    static Logger& instance() noexcept;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    void set_sink(Sink sink);
    void set_level(LogLevel level) noexcept;
    [[nodiscard]] LogLevel level() const noexcept;

    void log(LogLevel level, std::string_view category, std::string_view message);

    // Shorthands
    void trace(std::string_view category, std::string_view m) { log(LogLevel::Trace, category, m); }
    void debug(std::string_view category, std::string_view m) { log(LogLevel::Debug, category, m); }
    void info (std::string_view category, std::string_view m) { log(LogLevel::Info,  category, m); }
    void warn (std::string_view category, std::string_view m) { log(LogLevel::Warn,  category, m); }
    void error(std::string_view category, std::string_view m) { log(LogLevel::Error, category, m); }
    void fatal(std::string_view category, std::string_view m) { log(LogLevel::Fatal, category, m); }

private:
    Logger();
    ~Logger();

    struct Impl;
    // RAII pImpl; C4251 on the unique_ptr member is a false positive for an
    // incomplete opaque pointee consumed only via non-template API.
#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif
    std::unique_ptr<Impl> impl_;
#ifdef _MSC_VER
#  pragma warning(pop)
#endif
};

ARIA_RUNTIME_API const char* level_name(LogLevel l) noexcept;

}  // namespace aria::runtime

#pragma once

// Minimal, dependency-free logging and assertion facility.
//
// Logging is intentionally synchronous and cheap rather than clever: the hot
// paths in this engine must not log at all, so throughput is a non-goal and
// "the message is on disk before we crash" is the property that matters.

#include <format>
#include <source_location>
#include <string_view>

namespace voxl {

enum class LogLevel { Trace, Debug, Info, Warn, Error, Fatal };

/// Routes a fully formatted message to every active sink. Prefer the
/// VOXL_LOG_* macros, which add the source location for free.
void logMessage(LogLevel level, std::string_view message,
                const std::source_location& where);

/// Messages below this level are discarded. Defaults to Info in release
/// builds and Trace in debug builds.
void setLogLevel(LogLevel level);
LogLevel logLevel() noexcept;

/// Mirrors all output to the given file in addition to the console. Passing an
/// empty path disables file logging. Returns false if the file cannot be
/// opened, in which case console logging continues unaffected.
bool setLogFile(std::string_view path);

/// Flushes and closes any open log file. Safe to call more than once.
void shutdownLogging();

namespace detail {
/// Captures the call site while still allowing a variadic format call. The
/// format string is checked at compile time by std::format.
template <typename... Args>
struct LogWithLocation {
    LogWithLocation(LogLevel level, std::format_string<Args...> fmt, Args&&... args,
                    const std::source_location& where = std::source_location::current())
    {
        if (level < logLevel()) {
            return;
        }
        logMessage(level, std::format(fmt, std::forward<Args>(args)...), where);
    }
};

template <typename... Args>
LogWithLocation(LogLevel, std::format_string<Args...>, Args&&...) -> LogWithLocation<Args...>;

[[noreturn]] void assertionFailed(std::string_view expression, std::string_view message,
                                  const std::source_location& where);
}  // namespace detail

}  // namespace voxl

#define VOXL_LOG_TRACE(...) ::voxl::detail::LogWithLocation(::voxl::LogLevel::Trace, __VA_ARGS__)
#define VOXL_LOG_DEBUG(...) ::voxl::detail::LogWithLocation(::voxl::LogLevel::Debug, __VA_ARGS__)
#define VOXL_LOG_INFO(...)  ::voxl::detail::LogWithLocation(::voxl::LogLevel::Info,  __VA_ARGS__)
#define VOXL_LOG_WARN(...)  ::voxl::detail::LogWithLocation(::voxl::LogLevel::Warn,  __VA_ARGS__)
#define VOXL_LOG_ERROR(...) ::voxl::detail::LogWithLocation(::voxl::LogLevel::Error, __VA_ARGS__)
#define VOXL_LOG_FATAL(...) ::voxl::detail::LogWithLocation(::voxl::LogLevel::Fatal, __VA_ARGS__)

/// Always-checked invariant. Survives into release builds because every use
/// site guards a condition whose violation means the world state is corrupt.
#define VOXL_CHECK(expr, ...)                                                        \
    do {                                                                             \
        if (!(expr)) [[unlikely]] {                                                  \
            ::voxl::detail::assertionFailed(#expr, ::std::format("" __VA_ARGS__),     \
                                            ::std::source_location::current());      \
        }                                                                            \
    } while (false)

/// Debug-only invariant, compiled out entirely in release builds.
#if VOXL_DEBUG
    #define VOXL_ASSERT(expr, ...) VOXL_CHECK(expr, __VA_ARGS__)
#else
    #define VOXL_ASSERT(expr, ...) ((void)0)
#endif

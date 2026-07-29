#include "core/Log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#if defined(_WIN32)
    #include <windows.h>
#endif

namespace voxl {
namespace {

// Worker threads log too, so every sink access is serialised. The mutex is
// only ever taken on a message that already passed the level filter.
std::mutex g_sinkMutex;
std::ofstream g_file;
std::atomic<LogLevel> g_level{VOXL_DEBUG ? LogLevel::Trace : LogLevel::Info};

constexpr std::string_view levelName(LogLevel level) noexcept
{
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "?????";
}

// ANSI colours. Windows 10+ terminals support these once VT processing is on,
// which we enable lazily on first use.
constexpr std::string_view levelColour(LogLevel level) noexcept
{
    switch (level) {
        case LogLevel::Trace: return "\x1b[90m";
        case LogLevel::Debug: return "\x1b[36m";
        case LogLevel::Info:  return "\x1b[32m";
        case LogLevel::Warn:  return "\x1b[33m";
        case LogLevel::Error: return "\x1b[31m";
        case LogLevel::Fatal: return "\x1b[1;41m";
    }
    return "";
}

bool enableConsoleColour()
{
#if defined(_WIN32)
    HANDLE handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD mode = 0;
    if (::GetConsoleMode(handle, &mode) == 0) {
        return false;  // Output is redirected to a file or pipe.
    }
    return ::SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
    return true;
#endif
}

const bool g_colourEnabled = enableConsoleColour();

/// Trims the build machine's absolute path down to something readable.
std::string_view shortFileName(const char* path) noexcept
{
    std::string_view view{path};
    const auto slash = view.find_last_of("/\\");
    return slash == std::string_view::npos ? view : view.substr(slash + 1);
}

/// Seconds since process start. Wall-clock timestamps are noise when the whole
/// point is correlating a stall with a frame number.
double secondsSinceStart() noexcept
{
    static const auto origin = std::chrono::steady_clock::now();
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - origin;
    return elapsed.count();
}

}  // namespace

void setLogLevel(LogLevel level)
{
    g_level.store(level, std::memory_order_relaxed);
}

LogLevel logLevel() noexcept
{
    return g_level.load(std::memory_order_relaxed);
}

bool setLogFile(std::string_view path)
{
    const std::scoped_lock lock{g_sinkMutex};
    if (g_file.is_open()) {
        g_file.close();
    }
    if (path.empty()) {
        return true;
    }

    std::error_code ec;
    const std::filesystem::path filePath{path};
    if (filePath.has_parent_path()) {
        std::filesystem::create_directories(filePath.parent_path(), ec);
    }

    g_file.open(filePath, std::ios::out | std::ios::trunc);
    return g_file.is_open();
}

void logMessage(LogLevel level, std::string_view message, const std::source_location& where)
{
    if (level < logLevel()) {
        return;
    }

    const auto file = shortFileName(where.file_name());
    const double time = secondsSinceStart();

    const std::scoped_lock lock{g_sinkMutex};

    if (g_colourEnabled) {
        std::fprintf(stdout, "%s[%8.3f] [%s]\x1b[0m %.*s \x1b[90m(%.*s:%u)\x1b[0m\n",
                     levelColour(level).data(), time, levelName(level).data(),
                     static_cast<int>(message.size()), message.data(),
                     static_cast<int>(file.size()), file.data(), where.line());
    } else {
        std::fprintf(stdout, "[%8.3f] [%s] %.*s (%.*s:%u)\n", time, levelName(level).data(),
                     static_cast<int>(message.size()), message.data(),
                     static_cast<int>(file.size()), file.data(), where.line());
    }

    // Every record is flushed, not just warnings. When stdout is redirected it
    // is fully buffered, so a crash or an external kill would otherwise discard
    // the entire log - precisely the situation the log exists for. Nothing on a
    // per-frame path logs, so the syscall cost never shows up in a frame.
    std::fflush(stdout);

    if (g_file.is_open()) {
        g_file << std::format("[{:8.3f}] [{}] {} ({}:{})\n", time, levelName(level), message,
                              file, where.line());
        g_file.flush();
    }
}

void shutdownLogging()
{
    const std::scoped_lock lock{g_sinkMutex};
    std::fflush(stdout);
    if (g_file.is_open()) {
        g_file.flush();
        g_file.close();
    }
}

namespace detail {

void assertionFailed(std::string_view expression, std::string_view message,
                     const std::source_location& where)
{
    const std::string text =
        message.empty()
            ? std::format("Assertion failed: {}", expression)
            : std::format("Assertion failed: {} - {}", expression, message);

    logMessage(LogLevel::Fatal, text, where);
    shutdownLogging();

#if defined(_WIN32) && VOXL_DEBUG
    if (::IsDebuggerPresent()) {
        __debugbreak();
    }
#endif
    std::abort();
}

}  // namespace detail
}  // namespace voxl

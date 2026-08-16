// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Thread-safe leveled logging with console + optional file sinks.
// Uses std::format; levels below HELIOS_LOG_MIN_LEVEL are compiled out.
#pragma once

#include <format>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>

namespace helios::core {

enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Off = 5,
};

[[nodiscard]] std::string_view toString(LogLevel level) noexcept;

class Log {
public:
    // Global logger. Never null.
    static Log& instance();

    Log();
    ~Log();

    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;

    void setLevel(LogLevel level) noexcept;
    [[nodiscard]] LogLevel level() const noexcept;

    // Optional file sink (appended). Returns error on failure.
    void setLogFile(std::string_view path);

    void log(LogLevel level, std::string_view message, std::source_location loc = std::source_location::current());

    // Convenience: enable ANSI colors on Windows console.
    void setUseColor(bool use) noexcept { use_color_ = use; }

private:
    mutable std::mutex mutex_;
    LogLevel level_ = LogLevel::Info;
    bool use_color_ = true;
    FILE* file_ = nullptr; // owned
};

// --- free functions --------------------------------------------------------

template <typename... Args>
void logTrace(std::format_string<Args...> fmt, Args&&... args) {
    if constexpr (static_cast<int>(LogLevel::Trace) >= static_cast<int>(LogLevel::Off)) {
        (void)fmt;
    } else {
        Log::instance().log(LogLevel::Trace, std::format(fmt, std::forward<Args>(args)...),
                            std::source_location::current());
    }
}

template <typename... Args>
void logDebug(std::format_string<Args...> fmt, Args&&... args) {
    if constexpr (static_cast<int>(LogLevel::Debug) >= static_cast<int>(LogLevel::Off)) {
        (void)fmt;
    } else {
        Log::instance().log(LogLevel::Debug, std::format(fmt, std::forward<Args>(args)...),
                            std::source_location::current());
    }
}

template <typename... Args>
void logInfo(std::format_string<Args...> fmt, Args&&... args) {
    if constexpr (static_cast<int>(LogLevel::Info) >= static_cast<int>(LogLevel::Off)) {
        (void)fmt;
    } else {
        Log::instance().log(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...),
                            std::source_location::current());
    }
}

template <typename... Args>
void logWarn(std::format_string<Args...> fmt, Args&&... args) {
    if constexpr (static_cast<int>(LogLevel::Warn) >= static_cast<int>(LogLevel::Off)) {
        (void)fmt;
    } else {
        Log::instance().log(LogLevel::Warn, std::format(fmt, std::forward<Args>(args)...),
                            std::source_location::current());
    }
}

template <typename... Args>
void logError(std::format_string<Args...> fmt, Args&&... args) {
    if constexpr (static_cast<int>(LogLevel::Error) >= static_cast<int>(LogLevel::Off)) {
        (void)fmt;
    } else {
        Log::instance().log(LogLevel::Error, std::format(fmt, std::forward<Args>(args)...),
                            std::source_location::current());
    }
}

} // namespace helios::core

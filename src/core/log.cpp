// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/core/log.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace helios::core {

std::string_view toString(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace: return "trace";
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warn: return "warn";
    case LogLevel::Error: return "error";
    case LogLevel::Off: return "off";
    }
    return "unknown";
}

Log& Log::instance() {
    static Log logger;
    return logger;
}

Log::Log() = default;

Log::~Log() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

void Log::setLevel(LogLevel level) noexcept {
    std::lock_guard lock(mutex_);
    level_ = level;
}

LogLevel Log::level() const noexcept {
    std::lock_guard lock(mutex_);
    return level_;
}

void Log::setLogFile(std::string_view path) {
    std::lock_guard lock(mutex_);
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    FILE* f = nullptr;
#ifdef _WIN32
    fopen_s(&f, std::string(path).c_str(), "a");
#else
    f = std::fopen(std::string(path).c_str(), "a");
#endif
    file_ = f;
}

namespace {
constexpr const char* kLevelColor(LogLevel level) {
    switch (level) {
    case LogLevel::Trace: return "\x1b[90m"; // bright black
    case LogLevel::Debug: return "\x1b[36m"; // cyan
    case LogLevel::Info: return "\x1b[32m";  // green
    case LogLevel::Warn: return "\x1b[33m";  // yellow
    case LogLevel::Error: return "\x1b[31m"; // red
    default: return "";
    }
}
} // namespace

void Log::log(LogLevel level, std::string_view message, std::source_location loc) {
    if (static_cast<int>(level) < static_cast<int>(level_)) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char ts[32] = {};
    std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm);

    std::string line = std::format("{} [{:5}] {} ({}:{})\n", ts, toString(level), message,
                                   loc.file_name(), loc.line());

    std::lock_guard lock(mutex_);
    if (use_color_) {
        std::fprintf(stderr, "%s%s\x1b[0m", kLevelColor(level), line.c_str());
    } else {
        std::fprintf(stderr, "%s", line.c_str());
    }
    if (file_ != nullptr) {
        std::fprintf(file_, "%s", line.c_str());
        std::fflush(file_);
    }
    std::fflush(stderr);
}

} // namespace helios::core

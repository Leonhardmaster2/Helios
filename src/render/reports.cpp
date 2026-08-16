// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/render/reports.hpp"

#include "helios/core/hash.hpp"
#include "helios/core/io.hpp"
#include "helios/core/log.hpp"
#include "helios/version.hpp"

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace helios::render {

namespace {

std::string utcTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string osName() {
#ifdef _WIN32
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll != nullptr) {
        auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (fn != nullptr && fn(&info) == 0) {
            return std::format("Windows {}.{} (build {})", info.dwMajorVersion, info.dwMinorVersion,
                               info.dwBuildNumber);
        }
    }
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "Unknown OS";
#endif
}

std::string cpuName() {
#ifdef _WIN32
    char buf[512] = {};
    DWORD size = sizeof(buf);
    if (GetEnvironmentVariableA("PROCESSOR_IDENTIFIER", buf, size) > 0) {
        return buf;
    }
    return "unknown CPU";
#else
    return "unknown CPU";
#endif
}

std::string compilerName() {
#if defined(_MSC_VER)
    return std::format("MSVC {}", _MSC_FULL_VER);
#elif defined(__clang__)
    return std::format("Clang {}.{}.{}", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__)
    return std::format("GCC {}.{}.{}", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
    return "unknown compiler";
#endif
}

} // namespace

std::string dependencyLockHash() {
    auto hash = core::sha256File("cmake/deps.lock.json");
    if (hash.failed()) {
        // The working directory may not be the repo root; try parent paths.
        for (auto const& p : {"../cmake/deps.lock.json", "../../cmake/deps.lock.json"}) {
            hash = core::sha256File(p);
            if (hash.ok()) break;
        }
    }
    return hash.ok() ? hash.value() : "unavailable";
}

std::string readSlangVersion(std::string_view shader_dir) {
    auto text = core::readTextFile(std::string(shader_dir) + "/slangc-version.txt");
    if (text.failed()) {
        return "unknown";
    }
    std::string line = text.value();
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    return line;
}

assets::EnvironmentInfo collectEnvironment(std::string_view resolution, bool validation_enabled) {
    assets::EnvironmentInfo env;
    env.helios_version = std::format("{}.{}.{}", heliosVersion().major, heliosVersion().minor,
                                     heliosVersion().patch);
    env.helios_git_hash = std::string(heliosGitHash());
    env.build_type = HELIOS_BUILD_TYPE;
    env.compiler = compilerName();
    env.cmake_version = HELIOS_CMAKE_VERSION;
    env.slang_version = readSlangVersion(HELIOS_SHADER_DIR);
    env.os_name = osName();
    env.cpu_name = cpuName();
    env.gpu_name = "queried-at-runtime";
    env.driver_version = "queried-at-runtime";
    env.resolution = std::string(resolution);
    env.quality_preset = "balanced";
    env.validation_layers = validation_enabled ? "enabled" : "disabled";
    env.timestamp_utc = utcTimestamp();
    env.dependency_lock_hash = dependencyLockHash();
    return env;
}

core::Result<void> writeReports(std::string_view report_dir, assets::EnvironmentInfo const& env,
                                std::string const& device_report_json, assets::PerformanceReport const& performance,
                                assets::EvidenceReport const& evidence) {
    std::error_code ec;
    fs::create_directories(std::string(report_dir), ec);
    if (ec) {
        return core::makeError(core::ErrorCode::Io, "cannot create report dir: " + std::string(report_dir));
    }

    // capabilities.json (device body passthrough).
    nlohmann::json cap = nlohmann::json::object();
    cap["format"] = assets::kFormatCapabilityReport;
    cap["schema_version"] = assets::kSchemaVersionCapabilityReport;
    cap["environment"] = assets::environmentToJson(env);
    cap["device"] = nlohmann::json::parse(device_report_json);

    auto r1 = assets::writeJsonReport((fs::path(std::string(report_dir)) / "capabilities.json").string(), cap);
    auto r2 = assets::writeJsonReport((fs::path(std::string(report_dir)) / "performance.json").string(),
                                      assets::performanceReportToJson(performance));
    auto r3 = assets::writeJsonReport((fs::path(std::string(report_dir)) / "evidence.json").string(),
                                      assets::evidenceReportToJson(evidence));
    if (r1.failed()) return r1.error();
    if (r2.failed()) return r2.error();
    if (r3.failed()) return r3.error();
    core::logInfo("Reports written to {}", report_dir);
    return core::Result<void>{};
}

} // namespace helios::render

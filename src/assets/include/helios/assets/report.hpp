// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Versioned evidence reports (schemas/capability-report.schema.json,
// schemas/performance-report.schema.json, schemas/evidence-report.schema.json).
// These are the M0 initial report schemas; later milestones extend them
// versioned, never in place.
#pragma once

#include "helios/assets/schema.hpp"
#include "helios/core/error.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace helios::assets {

// ---- environment/scenario manifest (common to all reports) ----------------
struct EnvironmentInfo {
    std::string helios_version;  // e.g. "0.0.1"
    std::string helios_git_hash; // build commit
    std::string build_type;      // Debug/RelWithDebInfo/Release
    std::string compiler;        // "MSVC 19.44.35228", "GCC 13.2", ...
    std::string cmake_version;
    std::string slang_version;        // pinned slangc version string
    std::string os_name;              // "Windows 11 build 26200", ...
    std::string cpu_name;             // e.g. "AMD Ryzen ..."
    std::string gpu_name;             // e.g. "NVIDIA GeForce RTX 5080"
    std::string driver_version;       // Vulkan driver version string
    std::string resolution;           // "1920x1080" or "headless"
    std::string quality_preset;       // "balanced" (candidate profile)
    std::string validation_layers;    // "enabled"/"disabled"/"unavailable"
    std::string timestamp_utc;        // ISO-8601 capture time
    std::string dependency_lock_hash; // SHA-256 of cmake/deps.lock.json
};

[[nodiscard]] nlohmann::json environmentToJson(EnvironmentInfo const& env);

// ---- capability report ----------------------------------------------------
struct CapabilityReport {
    std::uint32_t schema_version = kSchemaVersionCapabilityReport;
    EnvironmentInfo environment;
    nlohmann::json body; // device/feature/limit facts (vk::DeviceReport serialized)
};

[[nodiscard]] nlohmann::json capabilityReportToJson(CapabilityReport const& report);

// ---- performance report ---------------------------------------------------
struct FramePercentiles {
    std::uint64_t frame_count = 0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
    double max_ms = 0.0;
    double min_ms = 0.0;
    double average_ms = 0.0;
};

struct PerformanceReport {
    std::uint32_t schema_version = kSchemaVersionPerformanceReport;
    EnvironmentInfo environment;
    FramePercentiles cpu_frame_ms; // CPU time per presented frame
    FramePercentiles gpu_frame_ms; // GPU time from timestamp queries
    nlohmann::json extras;         // per-path timings (upload/compute/...)
};

[[nodiscard]] nlohmann::json performanceReportToJson(PerformanceReport const& report);

// ---- evidence report ------------------------------------------------------
struct SmokePathResult {
    std::string name;       // e.g. "triangle"
    std::string status;     // "pass" | "fail" | "skipped"
    std::string detail;     // human-readable evidence summary
    nlohmann::json metrics; // machine-readable measurements
};

struct EvidenceReport {
    std::uint32_t schema_version = kSchemaVersionEvidenceReport;
    EnvironmentInfo environment;
    std::vector<SmokePathResult> smoke_paths;
    std::uint32_t validation_error_count = 0;
    std::uint32_t validation_warning_count = 0;
    std::vector<std::string> notes; // blocked/unvalidated items with reasons
};

[[nodiscard]] nlohmann::json evidenceReportToJson(EvidenceReport const& report);

// Write helpers (atomic).
[[nodiscard]] core::Result<void> writeJsonReport(std::string_view path, nlohmann::json const& json);

} // namespace helios::assets

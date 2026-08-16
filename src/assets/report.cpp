// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/assets/report.hpp"

#include "helios/core/io.hpp"

namespace helios::assets {

nlohmann::json environmentToJson(EnvironmentInfo const& env) {
    nlohmann::json j = nlohmann::json::object();
    j["helios_version"] = env.helios_version;
    j["helios_git_hash"] = env.helios_git_hash;
    j["build_type"] = env.build_type;
    j["compiler"] = env.compiler;
    j["cmake_version"] = env.cmake_version;
    j["slang_version"] = env.slang_version;
    j["os_name"] = env.os_name;
    j["cpu_name"] = env.cpu_name;
    j["gpu_name"] = env.gpu_name;
    j["driver_version"] = env.driver_version;
    j["resolution"] = env.resolution;
    j["quality_preset"] = env.quality_preset;
    j["validation_layers"] = env.validation_layers;
    j["timestamp_utc"] = env.timestamp_utc;
    j["dependency_lock_hash"] = env.dependency_lock_hash;
    return j;
}

nlohmann::json capabilityReportToJson(CapabilityReport const& report) {
    nlohmann::json j = nlohmann::json::object();
    j["format"] = kFormatCapabilityReport;
    j["schema_version"] = report.schema_version;
    j["environment"] = environmentToJson(report.environment);
    j["device"] = report.body;
    return j;
}

nlohmann::json performanceReportToJson(PerformanceReport const& report) {
    auto percentilesToJson = [](FramePercentiles const& p) {
        nlohmann::json j = nlohmann::json::object();
        j["frame_count"] = p.frame_count;
        j["p50_ms"] = p.p50_ms;
        j["p95_ms"] = p.p95_ms;
        j["p99_ms"] = p.p99_ms;
        j["max_ms"] = p.max_ms;
        j["min_ms"] = p.min_ms;
        j["average_ms"] = p.average_ms;
        return j;
    };
    nlohmann::json j = nlohmann::json::object();
    j["format"] = kFormatPerformanceReport;
    j["schema_version"] = report.schema_version;
    j["environment"] = environmentToJson(report.environment);
    j["cpu_frame_ms"] = percentilesToJson(report.cpu_frame_ms);
    j["gpu_frame_ms"] = percentilesToJson(report.gpu_frame_ms);
    j["extras"] = report.extras;
    return j;
}

nlohmann::json evidenceReportToJson(EvidenceReport const& report) {
    nlohmann::json j = nlohmann::json::object();
    j["format"] = kFormatEvidenceReport;
    j["schema_version"] = report.schema_version;
    j["environment"] = environmentToJson(report.environment);
    nlohmann::json paths = nlohmann::json::array();
    for (auto const& p : report.smoke_paths) {
        nlohmann::json e = nlohmann::json::object();
        e["name"] = p.name;
        e["status"] = p.status;
        e["detail"] = p.detail;
        e["metrics"] = p.metrics;
        paths.push_back(std::move(e));
    }
    j["smoke_paths"] = std::move(paths);
    j["validation_error_count"] = report.validation_error_count;
    j["validation_warning_count"] = report.validation_warning_count;
    nlohmann::json notes = nlohmann::json::array();
    for (auto const& n : report.notes) {
        notes.push_back(n);
    }
    j["notes"] = std::move(notes);
    return j;
}

core::Result<void> writeJsonReport(std::string_view path, nlohmann::json const& json) {
    return core::writeTextFileAtomic(path, json.dump(2));
}

} // namespace helios::assets

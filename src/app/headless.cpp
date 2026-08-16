// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// --headless-smoke: create a Vulkan device without presentation, run the
// bounded smoke suite (clear/triangle/depth/compute/transfer/timestamp),
// and emit machine-readable evidence reports (docs/ARCHITECTURE.md —
// Headless modes, item 1).
#include "helios/app/headless.hpp"

#include "helios/assets/report.hpp"
#include "helios/core/log.hpp"
#include "helios/render/reports.hpp"
#include "helios/vk/device.hpp"
#include "helios/vk/instance.hpp"
#include "helios/vk/smoke/smoke_suite.hpp"

#include <chrono>
#include <filesystem>
#include <format>
#include <string>

namespace fs = std::filesystem;

namespace helios::app {

int runHeadlessSmoke(CliOptions const& options) {
    core::logInfo("=== headless smoke ===");

    vk::InstanceOptions instance_options;
    instance_options.enable_validation = options.validation;
    instance_options.require_validation = false;
    auto instance = vk::Instance::create(instance_options);
    if (instance.failed()) {
        core::logError("instance creation failed: {}", instance.error().to_string());
        return 2;
    }

    vk::DeviceOptions device_options;
    device_options.require_swapchain = false;
    auto device = vk::Device::create(*instance.value(), std::nullopt, device_options);
    if (device.failed()) {
        core::logError("device creation failed: {}", device.error().to_string());
        return 2;
    }

    vk::smoke::SmokeOptions smoke_options;
    smoke_options.clear = options.smoke_clear;
    smoke_options.triangle = options.smoke_triangle;
    smoke_options.depth = options.smoke_depth;
    smoke_options.compute = options.smoke_compute;
    smoke_options.transfer = options.smoke_transfer;
    smoke_options.timestamp = options.smoke_timestamp;
    smoke_options.iterations = options.iterations;
    smoke_options.width = options.width;
    smoke_options.height = options.height;

    auto smoke = vk::smoke::HeadlessSmoke::create(*instance.value(), *device.value(), smoke_options);
    if (smoke.failed()) {
        core::logError("smoke suite setup failed: {}", smoke.error().to_string());
        return 2;
    }

    // Warm-up device (shader/pipeline compilation is not a measurement).
    device.value()->waitIdle();

    // Iterations.
    std::vector<assets::SmokePathResult> path_results;
    std::size_t failures = 0;
    for (std::uint32_t iter = 0; iter < std::max(1u, options.iterations); ++iter) {
        auto results = smoke.value()->run();
        if (iter == 0) {
            path_results.reserve(results.size());
            for (auto const& r : results) {
                assets::SmokePathResult pr;
                pr.name = r.name;
                pr.status = r.pass ? "pass" : "fail";
                pr.detail = r.detail;
                pr.metrics = r.metrics;
                path_results.push_back(std::move(pr));
                if (!r.pass) ++failures;
            }
        } else {
            // Later iterations only verify stability (must keep passing).
            for (auto const& r : results) {
                if (!r.pass) ++failures;
            }
        }
    }

    for (auto const& r : path_results) {
        core::logInfo("smoke path [{}] {} — {}", r.name, r.status, r.detail);
    }

    const auto& stats = instance.value()->validationStats();
    core::logInfo("validation: {} errors, {} warnings", stats.error_count, stats.warning_count);

    // ---- evidence reports ----
    const bool validation_enabled = instance.value()->validationEnabled();
    assets::EnvironmentInfo env = render::collectEnvironment(
        std::format("{}x{}", options.width, options.height), validation_enabled);
    env.gpu_name = device.value()->report().device_name;
    env.driver_version = device.value()->report().driver_info;

    assets::PerformanceReport performance;
    performance.environment = env;

    assets::EvidenceReport evidence;
    evidence.environment = env;
    evidence.smoke_paths = path_results;
    evidence.validation_error_count = stats.error_count;
    evidence.validation_warning_count = stats.warning_count;
    if (!validation_enabled) {
        evidence.notes.push_back("validation layers disabled for this run");
    }
    if (failures > 0) {
        evidence.notes.push_back(std::format("{} smoke path failures across all iterations", failures));
    }

    const std::string device_json = device.value()->reportJson().dump(2);
    auto write_result = render::writeReports(options.report_dir, env, device_json, performance, evidence);
    if (write_result.failed()) {
        core::logError("report writing failed: {}", write_result.error().to_string());
        return 2;
    }

    // Zero known validation errors is a hard M0 gate (docs/VALIDATION.md P-012).
    const bool validation_clean = stats.error_count == 0;
    const bool all_pass = failures == 0 && validation_clean;
    if (!validation_clean) {
        core::logError("FAIL: {} Vulkan validation errors present", stats.error_count);
    }

    core::logInfo("headless smoke: {}", all_pass ? "ALL PATHS PASS" : "FAILURES PRESENT");
    return all_pass ? 0 : 1;
}

} // namespace helios::app

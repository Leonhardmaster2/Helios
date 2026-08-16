// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/render/headless.hpp"

#include "helios/assets/report.hpp"
#include "helios/core/log.hpp"
#include "helios/render/reports.hpp"
#include "helios/vk/device.hpp"
#include "helios/vk/instance.hpp"
#include "helios/vk/smoke/smoke_suite.hpp"

#include <algorithm>
#include <format>
#include <string>

namespace helios::render {

core::Result<HeadlessSmokeOutcome> runHeadlessSmoke(HeadlessSmokeOptions const& options) {
    core::logInfo("=== headless smoke ({}x{}, {} iteration(s)) ===", options.width, options.height,
                  options.iterations);

    vk::InstanceOptions instance_options;
    instance_options.enable_validation = options.enable_validation;
    instance_options.require_validation = false;
    auto instance = vk::Instance::create(instance_options);
    if (instance.failed()) {
        return instance.error();
    }

    vk::DeviceOptions device_options;
    device_options.require_swapchain = false;
    auto device = vk::Device::create(*instance.value(), std::nullopt, device_options);
    if (device.failed()) {
        return device.error();
    }

    vk::smoke::SmokeOptions smoke_options;
    smoke_options.clear = options.clear;
    smoke_options.triangle = options.triangle;
    smoke_options.depth = options.depth;
    smoke_options.compute = options.compute;
    smoke_options.transfer = options.transfer;
    smoke_options.timestamp = options.timestamp;
    smoke_options.iterations = options.iterations;
    smoke_options.width = options.width;
    smoke_options.height = options.height;

    auto smoke = vk::smoke::HeadlessSmoke::create(*instance.value(), *device.value(), smoke_options);
    if (smoke.failed()) {
        return smoke.error();
    }

    device.value()->waitIdle(); // shader/pipeline warm-up is not a measurement

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
    assets::EnvironmentInfo env = collectEnvironment(
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
    auto write_result = writeReports(options.report_dir, env, device_json, performance, evidence);
    if (write_result.failed()) {
        return write_result.error();
    }

    const bool validation_clean = stats.error_count == 0;
    const bool all_pass = failures == 0 && validation_clean;
    if (!validation_clean) {
        core::logError("FAIL: {} Vulkan validation errors present", stats.error_count);
    }
    core::logInfo("headless smoke: {}", all_pass ? "ALL PATHS PASS" : "FAILURES PRESENT");

    HeadlessSmokeOutcome outcome;
    outcome.all_pass = all_pass;
    outcome.failed_paths = failures;
    outcome.validation_errors = stats.error_count;
    outcome.validation_warnings = stats.warning_count;
    for (auto const& r : path_results) {
        outcome.path_status.push_back(r.status);
    }
    return outcome;
}

} // namespace helios::render

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Evidence report assembly: environment manifest + capability/performance/
// evidence reports written into the milestone evidence directory.
#pragma once

#include "helios/assets/report.hpp"
#include "helios/core/error.hpp"

#include <string>
#include <vector>

namespace helios::render {

// Collects the environment/scenario manifest (compiler, OS, GPU, versions).
[[nodiscard]] assets::EnvironmentInfo collectEnvironment(std::string_view resolution, bool validation_enabled);

// SHA-256 of cmake/deps.lock.json (dependency lock hash for the manifest).
[[nodiscard]] std::string dependencyLockHash();

// Writes capabilities.json, performance.json and smoke-results.json +
// evidence.json under report_dir (created if missing). Returns the number
// of failed smoke paths (0 = all pass) via out_failures.
[[nodiscard]] core::Result<void> writeReports(
    std::string_view report_dir, assets::EnvironmentInfo const& env, std::string const& device_report_json,
    assets::PerformanceReport const& performance, assets::EvidenceReport const& evidence);

// Reads slangc-version.txt from the shader dir (best effort).
[[nodiscard]] std::string readSlangVersion(std::string_view shader_dir);

} // namespace helios::render

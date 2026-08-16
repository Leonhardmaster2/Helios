// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Swapchain-free smoke runner. Lives in helios_render (not helios_app) so
// the app keeps no helios_vk link edge (docs/ARCHITECTURE.md).
#pragma once

#include "helios/core/error.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace helios::render {

struct HeadlessSmokeOptions {
    std::uint32_t width = 640;
    std::uint32_t height = 480;
    std::uint32_t iterations = 3;
    bool enable_validation = true;
    bool clear = true;
    bool triangle = true;
    bool depth = true;
    bool compute = true;
    bool transfer = true;
    bool timestamp = true;
    std::string report_dir = "evidence/m0/local";
};

struct HeadlessSmokeOutcome {
    bool all_pass = false;
    std::size_t failed_paths = 0;
    std::uint32_t validation_errors = 0;
    std::uint32_t validation_warnings = 0;
    std::vector<std::string> path_status; // "pass"/"fail" per path
};

// Creates a device without presentation, runs the bounded smoke suite and
// writes capabilities.json / performance.json / evidence.json.
[[nodiscard]] core::Result<HeadlessSmokeOutcome> runHeadlessSmoke(HeadlessSmokeOptions const& options);

} // namespace helios::render

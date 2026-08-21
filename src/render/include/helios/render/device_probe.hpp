// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Minimal headless device probe for evidence tooling: creates a Vulkan
// instance (+validation layers when requested) and a swapchain-free device,
// and reports the queried capabilities relevant to later milestones.
#pragma once

#include "helios/core/error.hpp"

#include <cstdint>
#include <string>

namespace helios::render {

struct DeviceProbeResult {
    std::string device_report_json; // vk::DeviceReport serialized (schema v1)
    std::string depth_format;       // queried depth choice, e.g. "D32_SFLOAT"
    std::string gpu_name;
    std::string driver_info;
    std::uint32_t validation_errors = 0;
    std::uint32_t validation_warnings = 0;
    bool validation_enabled = false;
};

[[nodiscard]] core::Result<DeviceProbeResult> probeDeviceHeadless(bool enable_validation);

} // namespace helios::render

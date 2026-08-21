// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/render/device_probe.hpp"

#include "helios/vk/device.hpp"
#include "helios/vk/instance.hpp"

namespace helios::render {

core::Result<DeviceProbeResult> probeDeviceHeadless(bool enable_validation) {
    vk::InstanceOptions instance_options;
    instance_options.enable_validation = enable_validation;
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

    DeviceProbeResult out;
    out.device_report_json = device.value()->reportJson().dump(2);
    switch (device.value()->report().depth_choice) {
    case vk::DepthFormatChoice::D32Sfloat:
        out.depth_format = "D32_SFLOAT";
        break;
    case vk::DepthFormatChoice::D32SfloatS8:
        out.depth_format = "D32_SFLOAT_S8_UINT";
        break;
    case vk::DepthFormatChoice::D24UnormS8:
        out.depth_format = "D24_UNORM_S8_UINT";
        break;
    default:
        out.depth_format = "unknown";
        break;
    }
    out.gpu_name = device.value()->report().device_name;
    out.driver_info = device.value()->report().driver_info;
    out.validation_enabled = instance.value()->validationEnabled();
    out.validation_errors = instance.value()->validationStats().error_count;
    out.validation_warnings = instance.value()->validationStats().warning_count;
    return out;
}

} // namespace helios::render

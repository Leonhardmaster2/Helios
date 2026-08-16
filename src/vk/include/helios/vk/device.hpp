// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Physical-device selection, feature enablement, device creation and queue
// management. All capability probing is explicit; nothing is assumed from
// device name or age (docs/SUPPORT.md).
#pragma once

#include "helios/core/error.hpp"
#include "helios/vk/instance.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace helios::vk {

// Depth-format fallback chain (accepted direction D-002: queried 32-bit
// float depth, D32_SFLOAT preferred).
enum class DepthFormatChoice : std::uint8_t {
    D32Sfloat = 0,   // preferred
    D32SfloatS8 = 1, // fallback: 32-bit float depth + stencil
    D24UnormS8 = 2,  // last resort: 24-bit depth + stencil
};

struct DeviceOptions {
    bool require_swapchain = true;   // false for headless
    bool require_validation_features = false;
    DepthFormatChoice depth_format = DepthFormatChoice::D32Sfloat;
    // If set, only devices with apiVersion >= this are considered.
    std::uint32_t min_api_version = VK_API_VERSION_1_3;
    std::optional<std::uint64_t> preferred_device_id; // exact deviceID match preferred
    std::string preferred_device_name_substring;      // e.g. "RTX" (scoring hint only)
};

// Queue family facts (no concurrency promises — see docs/ARCHITECTURE.md).
struct QueueFamilyInfo {
    std::uint32_t family_index = 0;
    std::uint32_t queue_count = 0;
    VkQueueFlags flags = 0;
    bool supports_graphics = false;
    bool supports_compute = false;
    bool supports_transfer = false;
    bool supports_present = false;
    std::uint32_t timestamp_valid_bits = 0;
};

struct DeviceReport {
    // Identity
    std::string device_name;
    std::string driver_name;
    std::string driver_info;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::uint32_t api_version = 0;
    std::uint32_t driver_version = 0;
    VkPhysicalDeviceType device_type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    // Memory
    std::uint64_t device_local_heap_bytes = 0;
    std::uint64_t host_visible_heap_bytes = 0;
    // Queues
    std::vector<QueueFamilyInfo> queue_families;
    std::uint32_t used_queue_family = 0;
    // Required/queried features (1 = available&enabled, 0 = unavailable)
    bool dynamic_rendering = false;
    bool synchronization2 = false;
    bool timeline_semaphore = false;
    bool buffer_device_address = false;
    bool descriptor_indexing = false;
    bool host_query_reset = false;
    bool shader_float64 = false;
    bool maintenance4 = false;
    // Timestamps
    float timestamp_period_ns = 0.0f;
    bool timestamp_compute_and_graphics = false;
    // Depth formats (feature availability)
    bool depth_d32_sfloat = false;
    bool depth_d32_sfloat_s8 = false;
    bool depth_d24_unorm_s8 = false;
    DepthFormatChoice depth_choice = DepthFormatChoice::D32Sfloat;
    std::string depth_choice_name;
    // Limits (selected — see capabilities.cpp for the full probe)
    std::uint32_t max_image_dimension_2d = 0;
    std::uint32_t max_bound_descriptor_sets = 0;
    std::uint32_t max_descriptor_set_bindings = 0;
    std::uint32_t max_descriptor_set_uniform_buffers = 0;
    std::uint32_t max_descriptor_set_storage_buffers = 0;
    std::uint32_t max_descriptor_set_sampled_images = 0;
    std::uint32_t max_descriptor_set_samplers = 0;
    std::uint32_t max_memory_allocation_count = 0;
    std::uint32_t max_push_constants_size = 0;
    std::uint32_t max_vertex_input_bindings = 0;
    std::uint32_t max_vertex_input_attributes = 0;
    std::uint32_t max_vertex_output_components = 0;
    std::uint32_t max_fragment_input_components = 0;
    std::uint32_t max_framebuffer_width = 0;
    std::uint32_t max_framebuffer_height = 0;
    std::uint32_t max_color_attachments = 0;
    std::uint64_t max_uniform_buffer_range = 0;
    std::uint64_t max_storage_buffer_range = 0;
    std::uint64_t min_uniform_buffer_offset_alignment = 0;
    std::uint64_t min_storage_buffer_offset_alignment = 0;
    std::uint32_t max_compute_shared_memory_size = 0;
    std::uint32_t max_compute_work_group_invocations = 0;
    // Extensions
    std::vector<std::string> available_device_extensions;
    std::vector<std::string> enabled_device_extensions;
    // Selection evidence
    std::string selection_reason;
    std::vector<std::string> rejected_devices; // name + reason
};

// Serialize to JSON (versioned; consumed by helios_render report assembly).
[[nodiscard]] nlohmann::json deviceReportToJson(DeviceReport const& report);

class Device {
public:
    ~Device();
    Device(Device const&) = delete;
    Device& operator=(Device const&) = delete;

    // Enumerates physical devices, scores them, verifies required features
    // and creates the logical device with the selected features enabled.
    [[nodiscard]] static core::Result<std::unique_ptr<Device>> create(
        Instance& instance, std::optional<VkSurfaceKHR> surface, DeviceOptions const& options);

    [[nodiscard]] VkDevice handle() const noexcept { return device_; }
    [[nodiscard]] VkPhysicalDevice physicalDevice() const noexcept { return physical_; }
    [[nodiscard]] Instance& instance() const noexcept { return *instance_; }
    [[nodiscard]] DeviceReport const& report() const noexcept { return report_; }

    // Serialized device facts (used by capability report assembly).
    [[nodiscard]] nlohmann::json reportJson() const;

    [[nodiscard]] VkQueue graphicsQueue() const noexcept { return graphics_queue_; }
    [[nodiscard]] std::uint32_t graphicsQueueFamily() const noexcept { return queue_family_; }
    [[nodiscard]] bool hasSeparateComputeQueue() const noexcept { return compute_queue_ != VK_NULL_HANDLE; }
    [[nodiscard]] VkQueue computeQueue() const noexcept { return compute_queue_; }
    [[nodiscard]] VkQueue transferQueue() const noexcept { return transfer_queue_; }

    // Waits until the device is idle (debug/teardown use only).
    void waitIdle() const;

    // vkSetDebugUtilsObjectNameEXT wrapper (no-op when extension absent).
    void setDebugName(VkObjectType type, std::uint64_t handle, std::string_view name) const;

    [[nodiscard]] VmaAllocator allocator() const noexcept { return allocator_; }

    // Loads SPIR-V bytes from disk into a shader module (caller destroys).
    [[nodiscard]] core::Result<VkShaderModule> loadShaderModule(std::string_view spv_path) const;

    [[nodiscard]] float timestampPeriodNanos() const noexcept { return report_.timestamp_period_ns; }

private:
    Device() = default;
    void destroy();

    Instance* instance_ = nullptr;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VkQueue compute_queue_ = VK_NULL_HANDLE;
    VkQueue transfer_queue_ = VK_NULL_HANDLE;
    bool has_debug_utils_ = false;
    DeviceReport report_;
};

} // namespace helios::vk

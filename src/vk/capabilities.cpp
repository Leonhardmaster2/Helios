// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Device capability probing: features, limits, formats, queues, memory.
// Every fact is queried; nothing is inferred from device name or age
// (docs/SUPPORT.md, D-004).
#include "helios/vk/device.hpp"

#include "helios/core/log.hpp"

#include <volk.h>

namespace helios::vk {

namespace {

// Queries the promoted feature structs Helios relies on (D-004).
struct QueriedFeatures {
    VkPhysicalDeviceVulkan13Features f13;
    VkPhysicalDeviceVulkan11Features f11;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline;
    VkPhysicalDeviceBufferDeviceAddressFeatures buffer_address;
    VkPhysicalDeviceHostQueryResetFeatures host_query_reset;
    VkPhysicalDeviceDescriptorIndexingFeatures descriptor_indexing;
};

QueriedFeatures queryFeatures(VkPhysicalDevice physical) {
    QueriedFeatures q{};
    q.f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    q.f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    q.timeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    q.buffer_address.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    q.host_query_reset.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES;
    q.descriptor_indexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    q.f13.pNext = &q.f11;
    q.f11.pNext = &q.timeline;
    q.timeline.pNext = &q.buffer_address;
    q.buffer_address.pNext = &q.host_query_reset;
    q.host_query_reset.pNext = &q.descriptor_indexing;
    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &q.f13;
    vkGetPhysicalDeviceFeatures2(physical, &f2);
    return q;
}

} // namespace

nlohmann::json deviceReportToJson(DeviceReport const& r) {
    nlohmann::json j = nlohmann::json::object();
    j["device_name"] = r.device_name;
    j["driver_name"] = r.driver_name;
    j["driver_info"] = r.driver_info;
    j["vendor_id"] = r.vendor_id;
    j["device_id"] = r.device_id;
    j["api_version"] = r.api_version;
    j["api_version_string"] = Instance::apiVersionString(r.api_version);
    j["driver_version"] = r.driver_version;
    j["device_type"] = static_cast<int>(r.device_type);
    j["device_local_heap_bytes"] = r.device_local_heap_bytes;
    j["host_visible_heap_bytes"] = r.host_visible_heap_bytes;
    nlohmann::json qf = nlohmann::json::array();
    for (auto const& q : r.queue_families) {
        nlohmann::json e = nlohmann::json::object();
        e["family_index"] = q.family_index;
        e["queue_count"] = q.queue_count;
        e["flags"] = q.flags;
        e["graphics"] = q.supports_graphics;
        e["compute"] = q.supports_compute;
        e["transfer"] = q.supports_transfer;
        e["present"] = q.supports_present;
        e["timestamp_valid_bits"] = q.timestamp_valid_bits;
        qf.push_back(std::move(e));
    }
    j["queue_families"] = std::move(qf);
    j["used_queue_family"] = r.used_queue_family;
    j["features"] = nlohmann::json{
        {"dynamic_rendering", r.dynamic_rendering},     {"synchronization2", r.synchronization2},
        {"timeline_semaphore", r.timeline_semaphore},   {"buffer_device_address", r.buffer_device_address},
        {"descriptor_indexing", r.descriptor_indexing}, {"host_query_reset", r.host_query_reset},
        {"shader_float64", r.shader_float64},           {"maintenance4", r.maintenance4}};
    j["timestamp_period_ns"] = r.timestamp_period_ns;
    j["timestamp_compute_and_graphics"] = r.timestamp_compute_and_graphics;
    j["depth_formats"] = nlohmann::json{{"D32_SFLOAT", r.depth_d32_sfloat},
                                        {"D32_SFLOAT_S8_UINT", r.depth_d32_sfloat_s8},
                                        {"D24_UNORM_S8_UINT", r.depth_d24_unorm_s8},
                                        {"choice", r.depth_choice_name}};
    j["limits"] =
        nlohmann::json{{"max_image_dimension_2d", r.max_image_dimension_2d},
                       {"max_bound_descriptor_sets", r.max_bound_descriptor_sets},
                       {"max_per_stage_resources", r.max_per_stage_resources},
                       {"max_descriptor_set_uniform_buffers", r.max_descriptor_set_uniform_buffers},
                       {"max_descriptor_set_storage_buffers", r.max_descriptor_set_storage_buffers},
                       {"max_descriptor_set_sampled_images", r.max_descriptor_set_sampled_images},
                       {"max_descriptor_set_samplers", r.max_descriptor_set_samplers},
                       {"max_memory_allocation_count", r.max_memory_allocation_count},
                       {"max_push_constants_size", r.max_push_constants_size},
                       {"max_vertex_input_bindings", r.max_vertex_input_bindings},
                       {"max_vertex_input_attributes", r.max_vertex_input_attributes},
                       {"max_vertex_output_components", r.max_vertex_output_components},
                       {"max_fragment_input_components", r.max_fragment_input_components},
                       {"max_framebuffer_width", r.max_framebuffer_width},
                       {"max_framebuffer_height", r.max_framebuffer_height},
                       {"max_color_attachments", r.max_color_attachments},
                       {"max_uniform_buffer_range", r.max_uniform_buffer_range},
                       {"max_storage_buffer_range", r.max_storage_buffer_range},
                       {"min_uniform_buffer_offset_alignment", r.min_uniform_buffer_offset_alignment},
                       {"min_storage_buffer_offset_alignment", r.min_storage_buffer_offset_alignment},
                       {"max_compute_shared_memory_size", r.max_compute_shared_memory_size},
                       {"max_compute_work_group_invocations", r.max_compute_work_group_invocations}};
    j["available_device_extensions"] = r.available_device_extensions;
    j["enabled_device_extensions"] = r.enabled_device_extensions;
    j["selection_reason"] = r.selection_reason;
    j["rejected_devices"] = r.rejected_devices;
    return j;
}

void probeDeviceReport(DeviceReport& r, VkPhysicalDevice physical, VkSurfaceKHR surface,
                       DeviceOptions const& options, std::string& rejection_reason) {

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical, &props);
    r.device_name = props.deviceName;
    r.vendor_id = props.vendorID;
    r.device_id = props.deviceID;
    r.api_version = props.apiVersion;
    r.driver_version = props.driverVersion;
    r.device_type = props.deviceType;
    r.driver_name = "unknown";
    r.driver_info = "unknown";

    // Driver identity via properties2.
    {
        VkPhysicalDeviceDriverProperties driver{};
        driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        VkPhysicalDeviceProperties2 p2{};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &driver;
        vkGetPhysicalDeviceProperties2(physical, &p2);
        r.driver_name = driver.driverName;
        r.driver_info = driver.driverInfo;
    }

    // API version gate.
    if (r.api_version < options.min_api_version) {
        rejection_reason = "apiVersion " + Instance::apiVersionString(r.api_version) + " < required " +
                           Instance::apiVersionString(options.min_api_version);
        return;
    }

    // Features.
    const auto f = queryFeatures(physical);
    VkPhysicalDeviceFeatures base{};
    vkGetPhysicalDeviceFeatures(physical, &base);
    r.shader_draw_parameters = f.f11.shaderDrawParameters == VK_TRUE;
    r.dynamic_rendering = f.f13.dynamicRendering == VK_TRUE;
    r.synchronization2 = f.f13.synchronization2 == VK_TRUE;
    r.timeline_semaphore = f.timeline.timelineSemaphore == VK_TRUE;
    r.buffer_device_address = f.buffer_address.bufferDeviceAddress == VK_TRUE;
    r.descriptor_indexing = f.descriptor_indexing.runtimeDescriptorArray == VK_TRUE &&
                            f.descriptor_indexing.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
    r.host_query_reset = f.host_query_reset.hostQueryReset == VK_TRUE;
    r.shader_float64 = base.shaderFloat64 == VK_TRUE;
    r.maintenance4 = f.f13.maintenance4 == VK_TRUE;

    const bool required_ok = r.dynamic_rendering && r.synchronization2 && r.timeline_semaphore &&
                             r.buffer_device_address && r.host_query_reset;
    if (!required_ok) {
        rejection_reason =
            "missing required features (dynamic_rendering=" + std::to_string(r.dynamic_rendering) +
            ", synchronization2=" + std::to_string(r.synchronization2) +
            ", timeline_semaphore=" + std::to_string(r.timeline_semaphore) +
            ", buffer_device_address=" + std::to_string(r.buffer_device_address) +
            ", host_query_reset=" + std::to_string(r.host_query_reset) + ")";
        return;
    }

    // Queue families + present support.
    std::uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qf_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &qf_count, qf.data());
    for (std::uint32_t i = 0; i < qf_count; ++i) {
        QueueFamilyInfo info;
        info.family_index = i;
        info.queue_count = qf[i].queueCount;
        info.flags = qf[i].queueFlags;
        info.supports_graphics = (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        info.supports_compute = (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
        info.supports_transfer = (qf[i].queueFlags & VK_QUEUE_TRANSFER_BIT) != 0;
        info.timestamp_valid_bits = qf[i].timestampValidBits;
        if (surface != VK_NULL_HANDLE) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &present);
            info.supports_present = present == VK_TRUE;
        } else {
            info.supports_present = true; // headless: no presentation needed
        }
        r.queue_families.push_back(info);
    }
    if (r.queue_families.empty()) {
        rejection_reason = "no queue families";
        return;
    }

    // Depth format availability.
    {
        auto hasDepthFeature = [&](VkFormat format) {
            VkFormatProperties fp{};
            vkGetPhysicalDeviceFormatProperties(physical, format, &fp);
            return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
        };
        r.depth_d32_sfloat = hasDepthFeature(VK_FORMAT_D32_SFLOAT);
        r.depth_d32_sfloat_s8 = hasDepthFeature(VK_FORMAT_D32_SFLOAT_S8_UINT);
        r.depth_d24_unorm_s8 = hasDepthFeature(VK_FORMAT_D24_UNORM_S8_UINT);
        if (options.depth_format == DepthFormatChoice::D32Sfloat && r.depth_d32_sfloat) {
            r.depth_choice = DepthFormatChoice::D32Sfloat;
        } else if (options.depth_format == DepthFormatChoice::D32SfloatS8 && r.depth_d32_sfloat_s8) {
            r.depth_choice = DepthFormatChoice::D32SfloatS8;
        } else if (r.depth_d32_sfloat) {
            r.depth_choice = DepthFormatChoice::D32Sfloat;
        } else if (r.depth_d32_sfloat_s8) {
            r.depth_choice = DepthFormatChoice::D32SfloatS8;
        } else if (r.depth_d24_unorm_s8) {
            r.depth_choice = DepthFormatChoice::D24UnormS8;
        } else {
            rejection_reason = "no supported 32-bit-float (or fallback) depth format";
            return;
        }
        r.depth_choice_name = r.depth_choice == DepthFormatChoice::D32Sfloat     ? "D32_SFLOAT"
                              : r.depth_choice == DepthFormatChoice::D32SfloatS8 ? "D32_SFLOAT_S8_UINT"
                                                                                 : "D24_UNORM_S8_UINT";
    }

    // Timestamps.
    r.timestamp_period_ns = props.limits.timestampPeriod;
    r.timestamp_compute_and_graphics = props.limits.timestampComputeAndGraphics == VK_TRUE;

    // Limits.
    auto const& L = props.limits;
    r.max_image_dimension_2d = L.maxImageDimension2D;
    r.max_bound_descriptor_sets = L.maxBoundDescriptorSets;
    r.max_descriptor_set_uniform_buffers = L.maxDescriptorSetUniformBuffers;
    r.max_descriptor_set_storage_buffers = L.maxDescriptorSetStorageBuffers;
    r.max_descriptor_set_sampled_images = L.maxDescriptorSetSampledImages;
    r.max_descriptor_set_samplers = L.maxDescriptorSetSamplers;
    r.max_memory_allocation_count = L.maxMemoryAllocationCount;
    r.max_per_stage_resources = L.maxPerStageResources;
    r.max_push_constants_size = L.maxPushConstantsSize;
    r.max_vertex_input_bindings = L.maxVertexInputBindings;
    r.max_vertex_input_attributes = L.maxVertexInputAttributes;
    r.max_vertex_output_components = L.maxVertexOutputComponents;
    r.max_fragment_input_components = L.maxFragmentInputComponents;
    r.max_framebuffer_width = L.maxFramebufferWidth;
    r.max_framebuffer_height = L.maxFramebufferHeight;
    r.max_color_attachments = L.maxColorAttachments;
    r.max_uniform_buffer_range = L.maxUniformBufferRange;
    r.max_storage_buffer_range = L.maxStorageBufferRange;
    r.min_uniform_buffer_offset_alignment = L.minUniformBufferOffsetAlignment;
    r.min_storage_buffer_offset_alignment = L.minStorageBufferOffsetAlignment;
    r.max_compute_shared_memory_size = L.maxComputeSharedMemorySize;
    r.max_compute_work_group_invocations = L.maxComputeWorkGroupInvocations;

    // Memory heaps (host visibility is a memory-TYPE property, not a
    // heap flag — Vulkan has no VK_MEMORY_HEAP_HOST_VISIBLE_BIT).
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(physical, &mem);
    for (std::uint32_t i = 0; i < mem.memoryHeapCount; ++i) {
        if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            r.device_local_heap_bytes += mem.memoryHeaps[i].size;
        }
        bool host_visible = false;
        for (std::uint32_t t = 0; t < mem.memoryTypeCount; ++t) {
            if (mem.memoryTypes[t].heapIndex == i &&
                (mem.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
                host_visible = true;
                break;
            }
        }
        if (host_visible) {
            r.host_visible_heap_bytes += mem.memoryHeaps[i].size;
        }
    }

    // Extensions.
    {
        std::uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> exts(count);
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, exts.data());
        for (auto const& e : exts) {
            r.available_device_extensions.push_back(e.extensionName);
        }
    }
}

} // namespace helios::vk

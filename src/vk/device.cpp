// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/vk/device.hpp"

#include "helios/core/debug_name.hpp"
#include "helios/core/io.hpp"
#include "helios/core/log.hpp"

#include <algorithm>
#include <vk_mem_alloc.h>
#include <volk.h>

namespace helios::vk {

// Forward declaration from capabilities.cpp (fills a DeviceReport).
void probeDeviceReport(DeviceReport& r, VkPhysicalDevice physical, VkSurfaceKHR surface,
                       DeviceOptions const& options, std::string& rejection_reason);

namespace {

int scoreDevice(DeviceReport const& report) {
    int score = 0;
    if (report.device_type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        score += 1000;
    if (report.device_type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
        score += 400;
    score += static_cast<int>(report.api_version >> 22U) * 100;           // API major
    score += static_cast<int>((report.api_version >> 12U) & 0x3FFU) * 10; // API minor
    score +=
        static_cast<int>(std::min<std::uint64_t>(report.device_local_heap_bytes >> 30U, 64)); // GiB capped
    // Prefer a single graphics+compute+transfer family (serialized baseline).
    for (auto const& q : report.queue_families) {
        if (q.supports_graphics && q.supports_compute && q.supports_transfer) {
            score += 50;
            break;
        }
    }
    return score;
}

} // namespace

core::Result<std::unique_ptr<Device>> Device::create(Instance& instance, std::optional<VkSurfaceKHR> surface,
                                                     DeviceOptions const& options) {
    std::uint32_t device_count = 0;
    if (vkEnumeratePhysicalDevices(instance.handle(), &device_count, nullptr) != VK_SUCCESS ||
        device_count == 0) {
        return core::makeError(core::ErrorCode::Unsupported, "no Vulkan physical devices found");
    }
    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance.handle(), &device_count, devices.data());

    struct Candidate {
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        DeviceReport report;
        int score = 0;
    };

    std::vector<Candidate> candidates;
    std::vector<std::string> rejected;
    for (VkPhysicalDevice physical : devices) {
        Candidate c;
        c.physical = physical;
        std::string reason;
        probeDeviceReport(c.report, physical, surface.value_or(VK_NULL_HANDLE), options, reason);
        if (!reason.empty()) {
            rejected.push_back(c.report.device_name + ": " + reason);
            continue;
        }
        // Present support check when a surface is required.
        if (surface.has_value()) {
            bool can_present = false;
            for (auto const& q : c.report.queue_families) {
                if (q.supports_graphics && q.supports_present)
                    can_present = true;
            }
            if (!can_present) {
                rejected.push_back(c.report.device_name + ": cannot present to the surface");
                continue;
            }
        }
        c.score = scoreDevice(c.report);
        candidates.push_back(std::move(c));
    }

    if (candidates.empty()) {
        std::string detail;
        for (auto const& r : rejected)
            detail += "  - " + r + "\n";
        return core::makeError(core::ErrorCode::Unsupported,
                               "no suitable Vulkan device; rejected:\n" + detail);
    }

    std::sort(candidates.begin(), candidates.end(),
              [](Candidate const& a, Candidate const& b) { return a.score > b.score; });
    Candidate const& chosen = candidates.front();

    auto device = std::unique_ptr<Device>(new Device());
    device->instance_ = &instance;
    device->physical_ = chosen.physical;
    device->report_ = chosen.report;
    device->report_.selection_reason =
        "scored " + std::to_string(chosen.score) + " (discrete GPU, API version, VRAM, unified queues)";
    device->report_.rejected_devices = rejected;

    // Queue family selection: prefer a single graphics+compute+transfer family.
    std::uint32_t queue_family = UINT32_MAX;
    for (auto const& q : chosen.report.queue_families) {
        if (q.supports_graphics && q.supports_compute && q.supports_transfer) {
            queue_family = q.family_index;
            break;
        }
    }
    if (queue_family == UINT32_MAX) {
        for (auto const& q : chosen.report.queue_families) {
            if (q.supports_graphics) {
                queue_family = q.family_index;
                break;
            }
        }
    }
    if (queue_family == UINT32_MAX) {
        return core::makeError(core::ErrorCode::Unsupported, "no graphics queue family found");
    }
    device->queue_family_ = queue_family;
    device->report_.used_queue_family = queue_family;

    const float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;

    // Required device extensions.
    std::vector<char const*> extensions;
    if (surface.has_value() || options.require_swapchain) {
        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    bool has_debug_utils = false;
    for (auto const& e : chosen.report.available_device_extensions) {
        if (e == VK_EXT_DEBUG_UTILS_EXTENSION_NAME)
            has_debug_utils = true;
    }
    if (has_debug_utils) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    device->has_debug_utils_ = has_debug_utils;
    device->report_.enabled_device_extensions.clear();
    for (char const* e : extensions) {
        device->report_.enabled_device_extensions.emplace_back(e);
    }

    // Features to enable (probed availability gates everything below).
    VkPhysicalDeviceVulkan13Features f13{};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.maintenance4 = VK_TRUE;

    // Shaders use SV_VertexID/SV_InstanceID -> SPIR-V DrawParameters
    // capability -> requires shaderDrawParameters (VUID-...-pCode-08740).
    VkPhysicalDeviceVulkan11Features f11{};
    f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    f11.shaderDrawParameters = VK_TRUE;

    VkPhysicalDeviceTimelineSemaphoreFeatures ftimeline{};
    ftimeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    ftimeline.timelineSemaphore = VK_TRUE;

    VkPhysicalDeviceBufferDeviceAddressFeatures faddress{};
    faddress.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    faddress.bufferDeviceAddress = VK_TRUE;

    VkPhysicalDeviceHostQueryResetFeatures fhqr{};
    fhqr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES;
    fhqr.hostQueryReset = VK_TRUE;

    VkPhysicalDeviceDescriptorIndexingFeatures fdi{};
    fdi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    fdi.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    fdi.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    fdi.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    fdi.descriptorBindingPartiallyBound = VK_TRUE;
    fdi.descriptorBindingVariableDescriptorCount = VK_TRUE;
    fdi.runtimeDescriptorArray = VK_TRUE;
    fdi.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;

    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f13;
    f13.pNext = &f11;
    f11.pNext = &ftimeline;
    ftimeline.pNext = &faddress;
    faddress.pNext = &fhqr;
    fhqr.pNext = &fdi;
    // The descriptor-indexing bits we request may not all be available on
    // older devices; enable them only when advertised (probed earlier).
    {
        VkPhysicalDeviceDescriptorIndexingFeatures probe{};
        probe.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
        VkPhysicalDeviceFeatures2 p2{};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        p2.pNext = &probe;
        vkGetPhysicalDeviceFeatures2(chosen.physical, &p2);
        fdi.shaderSampledImageArrayNonUniformIndexing = probe.shaderSampledImageArrayNonUniformIndexing;
        fdi.descriptorBindingSampledImageUpdateAfterBind = probe.descriptorBindingSampledImageUpdateAfterBind;
        fdi.descriptorBindingStorageBufferUpdateAfterBind =
            probe.descriptorBindingStorageBufferUpdateAfterBind;
        fdi.descriptorBindingPartiallyBound = probe.descriptorBindingPartiallyBound;
        fdi.descriptorBindingVariableDescriptorCount = probe.descriptorBindingVariableDescriptorCount;
        fdi.runtimeDescriptorArray = probe.runtimeDescriptorArray;
        fdi.shaderStorageBufferArrayNonUniformIndexing = probe.shaderStorageBufferArrayNonUniformIndexing;
    }

    VkDeviceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.pNext = &f2;
    create_info.queueCreateInfoCount = 1;
    create_info.pQueueCreateInfos = &queue_info;
    create_info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.data();

    const VkResult result = vkCreateDevice(chosen.physical, &create_info, nullptr, &device->device_);
    if (result != VK_SUCCESS) {
        return core::makeError(core::ErrorCode::Vulkan,
                               "vkCreateDevice failed: " + std::to_string(static_cast<int>(result)));
    }
    volkLoadDevice(device->device_);
    vkGetDeviceQueue(device->device_, queue_family, 0, &device->graphics_queue_);

    // Serialized baseline (docs/ARCHITECTURE.md): only the enabled graphics
    // family exists on this device; compute/transfer run on the graphics
    // queue. Separate families would have to be enabled at device creation
    // and are an M4-era optimization, not a correctness dependency.
    device->compute_queue_ = device->graphics_queue_;
    device->transfer_queue_ = device->graphics_queue_;

    // VMA allocator. VMA 3.x dynamic mode (volk integration) requires the
    // proc-addr functions to be passed via pVulkanFunctions; other members
    // are fetched from the instance/device by VMA itself.
    VmaVulkanFunctions vma_functions{};
    vma_functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vma_functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo vma_info{};
    vma_info.vulkanApiVersion = std::min(chosen.report.api_version, VK_API_VERSION_1_3);
    vma_info.physicalDevice = chosen.physical;
    vma_info.device = device->device_;
    vma_info.instance = instance.handle();
    vma_info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    vma_info.pVulkanFunctions = &vma_functions;
    const VkResult vma_result = vmaCreateAllocator(&vma_info, &device->allocator_);
    if (vma_result != VK_SUCCESS) {
        return core::makeError(core::ErrorCode::Vulkan,
                               "vmaCreateAllocator failed: " + std::to_string(static_cast<int>(vma_result)));
    }

    core::logInfo("Device selected: {} ({}), API {}, driver {}", device->report_.device_name,
                  device->report_.driver_info, Instance::apiVersionString(device->report_.api_version),
                  device->report_.driver_version);
    core::logInfo("Depth format: {}, timestamp period {} ns, unified queue family {}",
                  device->report_.depth_choice_name, device->report_.timestamp_period_ns, queue_family);
    return device;
}

void Device::destroy() {
    if (allocator_ != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator_);
        allocator_ = VK_NULL_HANDLE;
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
}

Device::~Device() {
    destroy();
}

void Device::waitIdle() const {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
}

void Device::setDebugName(VkObjectType type, std::uint64_t handle, std::string_view name) const {
    if (!has_debug_utils_ || handle == 0) {
        return;
    }
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType = type;
    info.objectHandle = handle;
    const std::string sanitized = core::sanitizeDebugName(name);
    info.pObjectName = sanitized.c_str();
    vkSetDebugUtilsObjectNameEXT(device_, &info);
}

core::Result<VkShaderModule> Device::loadShaderModule(std::string_view spv_path) const {
    auto bytes = core::readBinaryFile(spv_path, 64U * 1024U * 1024U);
    if (bytes.failed()) {
        return bytes.error();
    }
    auto const& data = bytes.value();
    if (data.size() < 4 || data.size() % 4 != 0) {
        return core::makeError(core::ErrorCode::CorruptData,
                               "SPIR-V file invalid size: " + std::string(spv_path));
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = data.size();
    info.pCode = reinterpret_cast<std::uint32_t const*>(data.data());
    VkShaderModule module = VK_NULL_HANDLE;
    const VkResult r = vkCreateShaderModule(device_, &info, nullptr, &module);
    if (r != VK_SUCCESS) {
        return core::makeError(core::ErrorCode::Vulkan,
                               "vkCreateShaderModule failed: " + std::to_string(static_cast<int>(r)));
    }
    return module;
}

nlohmann::json Device::reportJson() const {
    return deviceReportToJson(report_);
}

} // namespace helios::vk

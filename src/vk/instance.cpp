// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/vk/instance.hpp"
#include "helios/vk/validation.hpp"

#include "helios/core/log.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>

#include <SDL3/SDL_vulkan.h>
#include <volk.h>

namespace helios::vk {

namespace {
std::once_flag g_volk_flag;
}

std::string Instance::apiVersionString(std::uint32_t version) {
    return std::to_string(VK_API_VERSION_MAJOR(version)) + "." +
           std::to_string(VK_API_VERSION_MINOR(version)) + "." +
           std::to_string(VK_API_VERSION_PATCH(version));
}

namespace {
VKAPI_ATTR VkBool32 VKAPI_CALL debugMessengerCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                      VkDebugUtilsMessageTypeFlagsEXT /*type*/,
                                                      VkDebugUtilsMessengerCallbackDataEXT const* data,
                                                      void* user_data) {
    // user_data points to Instance::validation_stats_ (owned by the
    // Instance, which outlives the messenger). NEVER point this at freed
    // memory: the validation layer may call the callback at any time.
    auto* stats = static_cast<ValidationStats*>(user_data);
    if (stats == nullptr || data == nullptr || data->pMessage == nullptr) {
        return VK_FALSE;
    }
    const std::string message(data->pMessage);

    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++stats->error_count;
        if (stats->last_errors.size() >= 32)
            stats->last_errors.pop_back();
        stats->last_errors.insert(stats->last_errors.begin(), message);
        core::logError("Vulkan validation ERROR: {}", message);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        ++stats->warning_count;
        if (stats->last_warnings.size() >= 32)
            stats->last_warnings.pop_back();
        stats->last_warnings.insert(stats->last_warnings.begin(), message);
        core::logWarn("Vulkan validation WARNING: {}", message);
    } else {
        ++stats->info_count;
        core::logDebug("Vulkan validation: {}", message);
    }
    return VK_FALSE;
}
} // namespace

core::Result<std::unique_ptr<Instance>> Instance::create(InstanceOptions const& options) {
    std::call_once(g_volk_flag, [] {
        const VkResult r = volkInitialize();
        if (r != VK_SUCCESS) {
            core::logError("volkInitialize failed: {}", static_cast<int>(r));
        }
    });

    auto instance = std::unique_ptr<Instance>(new Instance());

    // Instance version (loader/runtime capability).
    {
        std::uint32_t version = VK_API_VERSION_1_0;
        vkEnumerateInstanceVersion(&version);
        instance->instance_version_ = version;
    }

    // Enumerate instance extensions/layers.
    {
        std::uint32_t count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        instance->extensions_.resize(count);
        vkEnumerateInstanceExtensionProperties(nullptr, &count, instance->extensions_.data());
        std::uint32_t layer_count = 0;
        vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
        instance->layers_.resize(layer_count);
        vkEnumerateInstanceLayerProperties(&layer_count, instance->layers_.data());
    }

    auto hasExtension = [&](std::string_view name) {
        return std::any_of(instance->extensions_.begin(), instance->extensions_.end(),
                           [&](VkExtensionProperties const& e) { return e.extensionName == name; });
    };
    auto hasLayer = [&](std::string_view name) {
        return std::any_of(instance->layers_.begin(), instance->layers_.end(),
                           [&](VkLayerProperties const& l) { return l.layerName == name; });
    };

    std::vector<char const*> extensions;
    extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#ifdef VK_USE_PLATFORM_WIN32_KHR
    extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
    extensions.push_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_MACOS_MVK)
    extensions.push_back(VK_MVK_MACOS_SURFACE_EXTENSION_NAME);
#endif
    if (hasExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        instance->has_debug_utils_ = true;
    }
    if (hasExtension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        instance->has_portability_enumeration_ = true;
    }

    std::vector<char const*> layers;
    if (options.enable_validation) {
        if (hasLayer("VK_LAYER_KHRONOS_validation")) {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            instance->validation_enabled_ = true;
            core::logInfo("Validation layer VK_LAYER_KHRONOS_validation enabled");
        } else if (options.require_validation) {
            return core::makeError(core::ErrorCode::Unsupported,
                                   "VK_LAYER_KHRONOS_validation requested but not available");
        } else {
            core::logWarn(
                "Validation layer VK_LAYER_KHRONOS_validation not available; continuing without it");
        }
    }

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = options.app_name.c_str();
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "Helios";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = std::min(options.api_version, instance->instance_version_);

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    create_info.ppEnabledLayerNames = layers.data();
    create_info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.data();
    // The portability flag is only legal when the portability-enumeration
    // extension is actually enabled (VUID-VkInstanceCreateInfo-flags-04828).
    if (instance->has_portability_enumeration_) {
        create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    const VkResult result = vkCreateInstance(&create_info, nullptr, &instance->instance_);
    if (result != VK_SUCCESS) {
        return core::makeError(core::ErrorCode::Vulkan,
                               "vkCreateInstance failed: " + std::to_string(static_cast<int>(result)));
    }
    volkLoadInstance(instance->instance_);

    if (instance->has_debug_utils_) {
        VkDebugUtilsMessengerCreateInfoEXT dbg{};
        dbg.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
        dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dbg.pfnUserCallback = debugMessengerCallback;
        // Lifetime: validation_stats_ is owned by the Instance and lives
        // until the messenger is destroyed in Instance::destroy().
        dbg.pUserData = &instance->validation_stats_;
        vkCreateDebugUtilsMessengerEXT(instance->instance_, &dbg, nullptr, &instance->messenger_);
    }

    core::logInfo("Vulkan instance created (API {} requested, runtime {})",
                  apiVersionString(app_info.apiVersion), apiVersionString(instance->instance_version_));
    return instance;
}

void Instance::destroy() {
    if (messenger_ != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
        messenger_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

Instance::~Instance() {
    destroy();
}

core::Result<VkSurfaceKHR> Instance::createSurface(SDL_Window* window) {
    if (window == nullptr) {
        return core::makeError(core::ErrorCode::InvalidArgument, "createSurface: null SDL_Window");
    }
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window, instance_, nullptr, &surface)) {
        return core::makeError(core::ErrorCode::Vulkan,
                               "SDL_Vulkan_CreateSurface failed: " + std::string(SDL_GetError()));
    }
    return surface;
}

} // namespace helios::vk

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Vulkan instance lifecycle: volk initialization, validation-layer
// integration, debug messenger, surface creation (SDL3 adapter).
// helios_vk owns all Vulkan instance-level state (docs/ARCHITECTURE.md).
#pragma once

#include "helios/core/error.hpp"

#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

struct SDL_Window; // external surface adapter (SDL3)

namespace helios::vk {

struct InstanceOptions {
    std::string app_name = "Helios";
    bool enable_validation = true;                  // request VK_LAYER_KHRONOS_validation
    bool require_validation = false;                // fail startup when unavailable
    std::uint32_t api_version = VK_API_VERSION_1_3; // requested instance API version
};

// Validation-layer telemetry (error/warning counts + bounded message log).
struct ValidationStats {
    std::uint32_t error_count = 0;
    std::uint32_t warning_count = 0;
    std::uint32_t info_count = 0;
    std::vector<std::string> last_errors;   // most recent first, bounded
    std::vector<std::string> last_warnings; // most recent first, bounded
};

class Instance {
  public:
    ~Instance();
    Instance(Instance const&) = delete;
    Instance& operator=(Instance const&) = delete;

    // Creates the instance (volk is initialized globally on first use).
    [[nodiscard]] static core::Result<std::unique_ptr<Instance>> create(InstanceOptions const& options);

    [[nodiscard]] VkInstance handle() const noexcept {
        return instance_;
    }
    [[nodiscard]] bool validationEnabled() const noexcept {
        return validation_enabled_;
    }

    // Creates a presentation surface from an SDL3 window (vk owns it).
    [[nodiscard]] core::Result<VkSurfaceKHR> createSurface(SDL_Window* window);

    // Validation-layer statistics since instance creation.
    [[nodiscard]] ValidationStats const& validationStats() const noexcept {
        return validation_stats_;
    }

    // Version of the Vulkan loader/runtime (vkEnumerateInstanceVersion).
    [[nodiscard]] std::uint32_t instanceVersion() const noexcept {
        return instance_version_;
    }

    // Human-readable API version, e.g. "1.3.296".
    [[nodiscard]] static std::string apiVersionString(std::uint32_t version);

    [[nodiscard]] std::vector<VkExtensionProperties> const& availableExtensions() const noexcept {
        return extensions_;
    }
    [[nodiscard]] std::vector<VkLayerProperties> const& availableLayers() const noexcept {
        return layers_;
    }

  private:
    Instance() = default;
    void destroy();

    VkInstance instance_ = VK_NULL_HANDLE;
    std::uint32_t instance_version_ = VK_API_VERSION_1_0;
    bool validation_enabled_ = false;
    bool has_debug_utils_ = false;
    bool has_portability_enumeration_ = false;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    ValidationStats validation_stats_;
    std::vector<VkExtensionProperties> extensions_;
    std::vector<VkLayerProperties> layers_;

    friend struct DebugMessengerState;
};

} // namespace helios::vk

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// RAII shader module loaded from offline-compiled SPIR-V.
#pragma once

#include "helios/core/error.hpp"
#include "helios/vk/device.hpp"

#include <vulkan/vulkan.h>

namespace helios::vk {

class ShaderModule {
public:
    ShaderModule() = default;
    [[nodiscard]] static core::Result<ShaderModule> create(Device& device, std::string_view spv_path,
                                                           std::string_view debug_name);
    ShaderModule(ShaderModule&& other) noexcept;
    ShaderModule& operator=(ShaderModule&& other) noexcept;
    ShaderModule(ShaderModule const&) = delete;
    ShaderModule& operator=(ShaderModule const&) = delete;
    ~ShaderModule();

    [[nodiscard]] VkShaderModule handle() const noexcept { return module_; }
    [[nodiscard]] bool valid() const noexcept { return module_ != VK_NULL_HANDLE; }

private:
    void destroy();
    Device* device_ = nullptr;
    VkShaderModule module_ = VK_NULL_HANDLE;
};

} // namespace helios::vk

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/vk/shader_module.hpp"

namespace helios::vk {

core::Result<ShaderModule> ShaderModule::create(Device& device, std::string_view spv_path,
                                                std::string_view debug_name) {
    auto module = device.loadShaderModule(spv_path);
    if (module.failed()) {
        return module.error();
    }
    ShaderModule out;
    out.device_ = &device;
    out.module_ = module.value();
    device.setDebugName(VK_OBJECT_TYPE_SHADER_MODULE, reinterpret_cast<std::uint64_t>(out.module_), debug_name);
    return out;
}

ShaderModule::ShaderModule(ShaderModule&& other) noexcept {
    *this = std::move(other);
}

ShaderModule& ShaderModule::operator=(ShaderModule&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = other.device_;
        module_ = other.module_;
        other.device_ = nullptr;
        other.module_ = VK_NULL_HANDLE;
    }
    return *this;
}

void ShaderModule::destroy() {
    if (module_ != VK_NULL_HANDLE && device_ != nullptr) {
        vkDestroyShaderModule(device_->handle(), module_, nullptr);
        module_ = VK_NULL_HANDLE;
    }
}

ShaderModule::~ShaderModule() {
    destroy();
}

} // namespace helios::vk

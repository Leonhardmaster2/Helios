// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/render/diligent_runtime.hpp"

#include "helios/core/log.hpp"

#include "EngineFactoryVk.h"
#include "RefCntAutoPtr.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>

#ifndef HELIOS_DILIGENT_COMMIT
#define HELIOS_DILIGENT_COMMIT "unknown"
#endif
#ifndef HELIOS_DILIGENT_CORE_COMMIT
#define HELIOS_DILIGENT_CORE_COMMIT "unknown"
#endif

namespace helios::render {
namespace {

std::atomic<std::uint32_t> g_warning_count{0};
std::atomic<std::uint32_t> g_error_count{0};

void DILIGENT_CALL_TYPE diligentMessageCallback(Diligent::DEBUG_MESSAGE_SEVERITY severity,
                                                Diligent::Char const* message,
                                                Diligent::Char const* function,
                                                Diligent::Char const* file, Diligent::Int32 line) {
    const char* text = message != nullptr ? message : "(no message)";
    switch (severity) {
    case Diligent::DEBUG_MESSAGE_SEVERITY_WARNING:
        ++g_warning_count;
        core::logWarn("Diligent: {}", text);
        break;
    case Diligent::DEBUG_MESSAGE_SEVERITY_ERROR:
    case Diligent::DEBUG_MESSAGE_SEVERITY_FATAL_ERROR:
        ++g_error_count;
        core::logError("Diligent: {}", text);
        break;
    default:
        core::logInfo("Diligent: {}", text);
        break;
    }
    (void)function;
    (void)file;
    (void)line;
}

bool validColor(std::array<float, 4> const& color) {
    return std::ranges::all_of(color, [](float value) {
        return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
    });
}

} // namespace

struct DiligentRuntime::Impl {
    Diligent::IEngineFactoryVk* factory = nullptr; // process-owned singleton
    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> immediate_context;
    DiligentRuntimeInfo runtime_info;
};

DiligentRuntime::DiligentRuntime() : impl_(std::make_unique<Impl>()) {}

DiligentRuntime::~DiligentRuntime() {
    if (impl_ != nullptr && impl_->immediate_context != nullptr) {
        impl_->immediate_context->WaitForIdle();
    }
    if (impl_ != nullptr && impl_->factory != nullptr) {
        impl_->factory->SetMessageCallback(nullptr);
    }
}

core::Result<std::unique_ptr<DiligentRuntime>>
DiligentRuntime::create(DiligentRuntimeCreateInfo const& info) {
    auto runtime = std::unique_ptr<DiligentRuntime>(new DiligentRuntime{});
    g_warning_count.store(0);
    g_error_count.store(0);

    runtime->impl_->factory = Diligent::GetEngineFactoryVk();
    if (runtime->impl_->factory == nullptr) {
        return core::makeError(core::ErrorCode::Initialization,
                               "Diligent Vulkan engine factory is unavailable");
    }
    runtime->impl_->factory->SetMessageCallback(diligentMessageCallback);

    Diligent::EngineVkCreateInfo create_info;
    create_info.EnableValidation = info.enable_validation;
    runtime->impl_->factory->CreateDeviceAndContextsVk(create_info, &runtime->impl_->device,
                                                       &runtime->impl_->immediate_context);
    if (runtime->impl_->device == nullptr || runtime->impl_->immediate_context == nullptr) {
        return core::makeError(core::ErrorCode::Initialization,
                               "Diligent failed to create a Vulkan device/context");
    }

    auto const& device_info = runtime->impl_->device->GetDeviceInfo();
    if (device_info.Type != Diligent::RENDER_DEVICE_TYPE_VULKAN) {
        return core::makeError(core::ErrorCode::Internal,
                               "Diligent created a non-Vulkan device in a Vulkan-only build");
    }
    auto const& adapter_info = runtime->impl_->device->GetAdapterInfo();
    auto const vk_version = runtime->impl_->factory->GetVulkanVersion();
    runtime->impl_->runtime_info.backend = "Diligent Vulkan";
    runtime->impl_->runtime_info.adapter_name = adapter_info.Description;
    runtime->impl_->runtime_info.diligent_commit = HELIOS_DILIGENT_COMMIT;
    runtime->impl_->runtime_info.diligent_core_commit = HELIOS_DILIGENT_CORE_COMMIT;
    runtime->impl_->runtime_info.vulkan_major = vk_version.Major;
    runtime->impl_->runtime_info.vulkan_minor = vk_version.Minor;
    runtime->impl_->runtime_info.local_memory_bytes = adapter_info.Memory.LocalMemory;
    runtime->impl_->runtime_info.host_visible_memory_bytes = adapter_info.Memory.HostVisibleMemory;

    return runtime;
}

core::Result<DiligentClearResult>
DiligentRuntime::clearAndReadback(std::uint32_t width, std::uint32_t height,
                                 std::array<float, 4> color) {
    if (width == 0 || height == 0 || width > 16384 || height > 16384) {
        return core::makeError(core::ErrorCode::InvalidArgument,
                               "offscreen dimensions must be in [1, 16384]");
    }
    if (!validColor(color)) {
        return core::makeError(core::ErrorCode::InvalidArgument,
                               "clear color components must be finite values in [0, 1]");
    }
    const std::uint64_t byte_count = static_cast<std::uint64_t>(width) * height * 4;
    if (byte_count > std::numeric_limits<std::size_t>::max()) {
        return core::makeError(core::ErrorCode::OutOfRange, "offscreen image is too large");
    }

    Diligent::TextureDesc target_desc;
    target_desc.Name = "Helios Diligent offscreen clear target";
    target_desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
    target_desc.Width = width;
    target_desc.Height = height;
    target_desc.Format = Diligent::TEX_FORMAT_RGBA8_UNORM;
    target_desc.BindFlags = Diligent::BIND_RENDER_TARGET;
    target_desc.Usage = Diligent::USAGE_DEFAULT;

    Diligent::RefCntAutoPtr<Diligent::ITexture> target;
    impl_->device->CreateTexture(target_desc, nullptr, &target);
    if (target == nullptr) {
        return core::makeError(core::ErrorCode::Vulkan,
                               "Diligent failed to create the offscreen render target");
    }
    Diligent::ITextureView* rtv = target->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET);
    if (rtv == nullptr) {
        return core::makeError(core::ErrorCode::Internal,
                               "Diligent render target has no default RTV");
    }

    impl_->immediate_context->SetRenderTargets(
        1, &rtv, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    impl_->immediate_context->ClearRenderTarget(
        rtv, color.data(), Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    impl_->immediate_context->SetRenderTargets(
        0, nullptr, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE);

    Diligent::TextureDesc staging_desc = target_desc;
    staging_desc.Name = "Helios Diligent offscreen readback";
    staging_desc.BindFlags = Diligent::BIND_NONE;
    staging_desc.Usage = Diligent::USAGE_STAGING;
    staging_desc.CPUAccessFlags = Diligent::CPU_ACCESS_READ;
    Diligent::RefCntAutoPtr<Diligent::ITexture> staging;
    impl_->device->CreateTexture(staging_desc, nullptr, &staging);
    if (staging == nullptr) {
        return core::makeError(core::ErrorCode::Vulkan,
                               "Diligent failed to create the staging readback texture");
    }

    Diligent::CopyTextureAttribs copy;
    copy.pSrcTexture = target;
    copy.SrcTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    copy.pDstTexture = staging;
    copy.DstTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    impl_->immediate_context->CopyTexture(copy);
    impl_->immediate_context->Flush();
    impl_->immediate_context->WaitForIdle();

    Diligent::MappedTextureSubresource mapped;
    impl_->immediate_context->MapTextureSubresource(staging, 0, 0, Diligent::MAP_READ,
                                                    Diligent::MAP_FLAG_NONE, nullptr, mapped);
    if (mapped.pData == nullptr || mapped.Stride < static_cast<Diligent::Uint64>(width) * 4) {
        if (mapped.pData != nullptr) {
            impl_->immediate_context->UnmapTextureSubresource(staging, 0, 0);
        }
        return core::makeError(core::ErrorCode::Vulkan,
                               "Diligent staging texture map returned invalid data");
    }

    DiligentClearResult result;
    result.width = width;
    result.height = height;
    result.rgba8.resize(static_cast<std::size_t>(byte_count));
    for (std::uint32_t y = 0; y < height; ++y) {
        auto const* src = static_cast<std::uint8_t const*>(mapped.pData) + y * mapped.Stride;
        std::memcpy(result.rgba8.data() + static_cast<std::size_t>(y) * width * 4, src,
                    static_cast<std::size_t>(width) * 4);
    }
    impl_->immediate_context->UnmapTextureSubresource(staging, 0, 0);
    return result;
}

DiligentRuntimeInfo DiligentRuntime::info() const {
    DiligentRuntimeInfo result = impl_->runtime_info;
    result.warning_count = g_warning_count.load();
    result.error_count = g_error_count.load();
    return result;
}

} // namespace helios::render

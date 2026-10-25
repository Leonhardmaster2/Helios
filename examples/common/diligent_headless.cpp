// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "diligent_headless.hpp"

#include <cstdio>
#include <cstring>

namespace helios::examples {

std::unique_ptr<DiligentHeadless> DiligentHeadless::create(bool enable_validation,
                                                           std::string& error) {
    if (active_instance_ != nullptr) {
        error = "only one headless Diligent device/context is allowed per process";
        return nullptr;
    }
    auto result = std::unique_ptr<DiligentHeadless>{new DiligentHeadless{}};
    active_instance_ = result.get();

    result->factory_ = Diligent::GetEngineFactoryVk();
    if (result->factory_ == nullptr) {
        error = "Diligent's statically linked Vulkan factory is unavailable";
        return nullptr;
    }
    result->factory_->SetMessageCallback(messageCallback);

    Diligent::EngineVkCreateInfo engine_info;
    engine_info.EnableValidation = enable_validation;
    // Dynamic buffers take their full size from this heap at every map; the
    // planet's bodies each map a 2 MB patch buffer per frame and the gas
    // solvers map theirs, so the default 8 MB stalls or fails.
    engine_info.DynamicHeapSize = 64u << 20;
    result->factory_->CreateDeviceAndContextsVk(engine_info, &result->device_, &result->context_);
    if (result->device_ == nullptr || result->context_ == nullptr) {
        error = "Diligent failed to create its Vulkan render device and immediate context";
        return nullptr;
    }
    if (result->device_->GetDeviceInfo().Type != Diligent::RENDER_DEVICE_TYPE_VULKAN) {
        error = "the headless lab device is not a Vulkan render device";
        return nullptr;
    }
    return result;
}

DiligentHeadless::~DiligentHeadless() {
    if (context_ != nullptr) {
        context_->SetRenderTargets(0, nullptr, nullptr,
                                   Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE);
        context_->WaitForIdle();
    }
    context_.Release();
    device_.Release();
    if (factory_ != nullptr) {
        factory_->SetMessageCallback(nullptr);
        factory_ = nullptr;
    }
    if (active_instance_ == this) {
        active_instance_ = nullptr;
    }
}

void DiligentHeadless::messageCallback(Diligent::DEBUG_MESSAGE_SEVERITY severity,
                                       Diligent::Char const* message, Diligent::Char const*,
                                       Diligent::Char const*, Diligent::Int32) {
    bool const is_warning = severity == Diligent::DEBUG_MESSAGE_SEVERITY_WARNING;
    bool const is_error = severity == Diligent::DEBUG_MESSAGE_SEVERITY_ERROR ||
                          severity == Diligent::DEBUG_MESSAGE_SEVERITY_FATAL_ERROR;
    if (!is_warning && !is_error) {
        return; // device-creation chatter is not useful in a batch run
    }
    char const* const text = message != nullptr ? message : "(no message)";
    std::fprintf(stderr, "Diligent %s: %s\n", is_error ? "error" : "warning", text);
    if (active_instance_ == nullptr) {
        return;
    }
    std::scoped_lock lock{active_instance_->diagnostics_mutex_};
    if (is_error) {
        ++active_instance_->error_count_;
    } else {
        ++active_instance_->warning_count_;
    }
    constexpr std::size_t kMessageLimit = 64;
    if (active_instance_->messages_.size() < kMessageLimit) {
        active_instance_->messages_.emplace_back(text);
    }
}

std::string DiligentHeadless::adapterName() const {
    return device_ != nullptr ? std::string{device_->GetAdapterInfo().Description} : std::string{};
}

std::uint32_t DiligentHeadless::warningCount() const noexcept {
    std::scoped_lock lock{diagnostics_mutex_};
    return warning_count_;
}

std::uint32_t DiligentHeadless::errorCount() const noexcept {
    std::scoped_lock lock{diagnostics_mutex_};
    return error_count_;
}

std::vector<std::string> DiligentHeadless::warningAndErrorMessages() const {
    std::scoped_lock lock{diagnostics_mutex_};
    return messages_;
}

Diligent::RefCntAutoPtr<Diligent::ITexture>
createColorTarget(Diligent::IRenderDevice* device, char const* name, std::uint32_t width,
                  std::uint32_t height, Diligent::TEXTURE_FORMAT format, bool shader_resource) {
    Diligent::TextureDesc desc;
    desc.Name = name;
    desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
    desc.Width = width;
    desc.Height = height;
    desc.Format = format;
    desc.Usage = Diligent::USAGE_DEFAULT;
    desc.BindFlags = shader_resource
                         ? Diligent::BIND_RENDER_TARGET | Diligent::BIND_SHADER_RESOURCE
                         : Diligent::BIND_RENDER_TARGET;
    Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
    device->CreateTexture(desc, nullptr, &texture);
    return texture;
}

bool readbackTexture(Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                     Diligent::ITexture* source, std::size_t bytes_per_pixel,
                     std::vector<std::uint8_t>& bytes, std::string& error) {
    if (device == nullptr || context == nullptr || source == nullptr || bytes_per_pixel == 0) {
        error = "readback requires a device, a context, a source texture and a pixel size";
        return false;
    }
    Diligent::TextureDesc const& source_desc = source->GetDesc();

    Diligent::TextureDesc staging_desc;
    staging_desc.Name = "Helios lab readback staging";
    staging_desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
    staging_desc.Width = source_desc.Width;
    staging_desc.Height = source_desc.Height;
    staging_desc.Format = source_desc.Format;
    staging_desc.BindFlags = Diligent::BIND_NONE;
    staging_desc.Usage = Diligent::USAGE_STAGING;
    staging_desc.CPUAccessFlags = Diligent::CPU_ACCESS_READ;
    Diligent::RefCntAutoPtr<Diligent::ITexture> staging;
    device->CreateTexture(staging_desc, nullptr, &staging);
    if (staging == nullptr) {
        error = "Diligent failed to create the staging readback texture";
        return false;
    }

    // The source may still be bound as a render target; unbind before copying.
    context->SetRenderTargets(0, nullptr, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE);
    Diligent::CopyTextureAttribs copy;
    copy.pSrcTexture = source;
    copy.SrcTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    copy.pDstTexture = staging;
    copy.DstTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    context->CopyTexture(copy);
    context->Flush();
    context->WaitForIdle();

    // The Vulkan backend never blocks in a staging map; WaitForIdle above is
    // the synchronization, and DO_NOT_WAIT states that explicitly.
    Diligent::MappedTextureSubresource mapped;
    context->MapTextureSubresource(staging, 0, 0, Diligent::MAP_READ,
                                   Diligent::MAP_FLAG_DO_NOT_WAIT, nullptr, mapped);
    std::size_t const row_bytes = static_cast<std::size_t>(source_desc.Width) * bytes_per_pixel;
    if (mapped.pData == nullptr || mapped.Stride < row_bytes) {
        if (mapped.pData != nullptr) {
            context->UnmapTextureSubresource(staging, 0, 0);
        }
        error = "Diligent staging texture map returned invalid data";
        return false;
    }
    bytes.resize(row_bytes * source_desc.Height);
    for (std::uint32_t row = 0; row < source_desc.Height; ++row) {
        auto const* const row_source =
            static_cast<std::uint8_t const*>(mapped.pData) + static_cast<std::size_t>(row) * mapped.Stride;
        std::memcpy(bytes.data() + static_cast<std::size_t>(row) * row_bytes, row_source, row_bytes);
    }
    context->UnmapTextureSubresource(staging, 0, 0);
    return true;
}

} // namespace helios::examples

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "fullscreen_pass.hpp"

#include "shaders_embedded.hpp"

#include <algorithm>
#include <string_view>

namespace helios::examples {

Diligent::RefCntAutoPtr<Diligent::IShader>
createEmbeddedShader(Diligent::IRenderDevice* device, char const* embedded_name, char const* label,
                     Diligent::SHADER_TYPE type, std::string& error) {
    unsigned char const* data = nullptr;
    std::size_t size = 0;
    for (std::size_t i = 0; i < shaders::embedded::kShaderCount; ++i) {
        auto const& shader = shaders::embedded::kShaders[i];
        if (shader.name != nullptr && std::string_view{embedded_name} == shader.name) {
            data = shader.data;
            size = shader.size;
            break;
        }
    }
    if (data == nullptr || (size & 3U) != 0) {
        error = std::string{"embedded SPIR-V is missing or misaligned: "} + embedded_name;
        return {};
    }
    Diligent::ShaderCreateInfo info;
    info.Desc.Name = label;
    info.Desc.ShaderType = type;
    info.EntryPoint = "main";
    info.ByteCode = data;
    info.ByteCodeSize = size;
    Diligent::RefCntAutoPtr<Diligent::IShader> shader;
    device->CreateShader(info, &shader);
    if (shader == nullptr) {
        error = std::string{"Diligent rejected the embedded SPIR-V: "} + embedded_name;
    }
    return shader;
}

Diligent::RefCntAutoPtr<Diligent::IPipelineState>
createFullscreenPipeline(Diligent::IRenderDevice* device, FullscreenPipelineInfo const& info) {
    Diligent::GraphicsPipelineStateCreateInfo pipeline_info;
    pipeline_info.PSODesc.Name = info.name;
    pipeline_info.PSODesc.PipelineType = Diligent::PIPELINE_TYPE_GRAPHICS;
    pipeline_info.PSODesc.ResourceLayout.DefaultVariableType =
        Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
    pipeline_info.PSODesc.ResourceLayout.Variables = info.variables.data();
    pipeline_info.PSODesc.ResourceLayout.NumVariables =
        static_cast<Diligent::Uint32>(info.variables.size());

    auto& graphics = pipeline_info.GraphicsPipeline;
    graphics.NumRenderTargets = static_cast<Diligent::Uint8>(info.target_formats.size());
    for (std::size_t index = 0; index < info.target_formats.size(); ++index) {
        graphics.RTVFormats[index] = info.target_formats[index];
    }
    graphics.DSVFormat = Diligent::TEX_FORMAT_UNKNOWN;
    graphics.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    graphics.RasterizerDesc.CullMode = Diligent::CULL_MODE_NONE;
    graphics.DepthStencilDesc.DepthEnable = false;
    graphics.DepthStencilDesc.DepthWriteEnable = false;
    if (info.blend != FullscreenBlend::Opaque) {
        auto& blend = graphics.BlendDesc.RenderTargets[0];
        blend.BlendEnable = true;
        blend.BlendOp = Diligent::BLEND_OPERATION_ADD;
        blend.BlendOpAlpha = Diligent::BLEND_OPERATION_ADD;
        if (info.blend == FullscreenBlend::Additive) {
            blend.SrcBlend = Diligent::BLEND_FACTOR_ONE;
            blend.DestBlend = Diligent::BLEND_FACTOR_ONE;
            blend.SrcBlendAlpha = Diligent::BLEND_FACTOR_ONE;
            blend.DestBlendAlpha = Diligent::BLEND_FACTOR_ZERO;
        } else if (info.blend == FullscreenBlend::PremultipliedOver) {
            blend.SrcBlend = Diligent::BLEND_FACTOR_ONE;
            blend.DestBlend = Diligent::BLEND_FACTOR_INV_SRC_ALPHA;
            blend.SrcBlendAlpha = Diligent::BLEND_FACTOR_ZERO;
            blend.DestBlendAlpha = Diligent::BLEND_FACTOR_ONE;
        } else {
            blend.SrcBlend = Diligent::BLEND_FACTOR_BLEND_FACTOR;
            blend.DestBlend = Diligent::BLEND_FACTOR_INV_BLEND_FACTOR;
            blend.SrcBlendAlpha = Diligent::BLEND_FACTOR_BLEND_FACTOR;
            blend.DestBlendAlpha = Diligent::BLEND_FACTOR_INV_BLEND_FACTOR;
        }
    }
    pipeline_info.pVS = info.vertex_shader;
    pipeline_info.pPS = info.pixel_shader;
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> pipeline;
    device->CreateGraphicsPipelineState(pipeline_info, &pipeline);
    return pipeline;
}

bool bindStaticVariable(Diligent::IPipelineState* pipeline, char const* name,
                        Diligent::IDeviceObject* object, std::string& error) {
    auto* variable = pipeline != nullptr
                         ? pipeline->GetStaticVariableByName(Diligent::SHADER_TYPE_PIXEL, name)
                         : nullptr;
    if (variable == nullptr) {
        error = std::string{"Diligent could not reflect the static shader variable '"} + name + "'";
        return false;
    }
    variable->Set(object);
    return true;
}

bool bindVariable(Diligent::IShaderResourceBinding* binding, char const* name,
                  Diligent::IDeviceObject* object, std::string& error) {
    auto* variable =
        binding != nullptr ? binding->GetVariableByName(Diligent::SHADER_TYPE_PIXEL, name) : nullptr;
    if (variable == nullptr) {
        error = std::string{"Diligent could not reflect the shader variable '"} + name + "'";
        return false;
    }
    variable->Set(object);
    return true;
}

Diligent::RefCntAutoPtr<Diligent::IBuffer>
createDynamicUniformBuffer(Diligent::IRenderDevice* device, char const* name, std::size_t size) {
    Diligent::BufferDesc desc;
    desc.Name = name;
    desc.Size = size;
    desc.BindFlags = Diligent::BIND_UNIFORM_BUFFER;
    desc.Usage = Diligent::USAGE_DYNAMIC;
    desc.CPUAccessFlags = Diligent::CPU_ACCESS_WRITE;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> buffer;
    device->CreateBuffer(desc, nullptr, &buffer);
    return buffer;
}

Diligent::RefCntAutoPtr<Diligent::ITexture>
createRenderTexture(Diligent::IRenderDevice* device, char const* name, std::uint32_t width,
                    std::uint32_t height, Diligent::TEXTURE_FORMAT format,
                    Diligent::ISampler* sampler) {
    Diligent::TextureDesc desc;
    desc.Name = name;
    desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.Usage = Diligent::USAGE_DEFAULT;
    desc.BindFlags = Diligent::BIND_RENDER_TARGET | Diligent::BIND_SHADER_RESOURCE;
    Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
    device->CreateTexture(desc, nullptr, &texture);
    if (texture != nullptr && sampler != nullptr) {
        texture->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE)->SetSampler(sampler);
    }
    return texture;
}

void drawFullscreen(Diligent::IDeviceContext* context,
                    std::span<Diligent::ITextureView* const> targets, std::uint32_t width,
                    std::uint32_t height, Diligent::IPipelineState* pipeline,
                    Diligent::IShaderResourceBinding* resources) {
    // SetRenderTargets takes a mutable array of view pointers.
    std::array<Diligent::ITextureView*, 4> views{};
    std::size_t const count = std::min(targets.size(), views.size());
    for (std::size_t index = 0; index < count; ++index) {
        views[index] = targets[index];
    }
    context->SetRenderTargets(static_cast<Diligent::Uint32>(count), views.data(), nullptr,
                              Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Diligent::Viewport const viewport{0.0f, 0.0f, static_cast<float>(width),
                                      static_cast<float>(height), 0.0f, 1.0f};
    context->SetViewports(1, &viewport, width, height);
    context->SetPipelineState(pipeline);
    context->CommitShaderResources(resources,
                                   Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Diligent::DrawAttribs draw;
    draw.NumVertices = 3;
    draw.Flags = Diligent::DRAW_FLAG_VERIFY_ALL;
    context->Draw(draw);
}

bool GpuPassTimer::initialize(Diligent::IRenderDevice* device, char const* name) {
    supported_ = device->GetDeviceInfo().Features.DurationQueries !=
                 Diligent::DEVICE_FEATURE_STATE_DISABLED;
    if (!supported_) {
        return false;
    }
    for (Slot& slot : slots_) {
        Diligent::QueryDesc desc{Diligent::QUERY_TYPE_DURATION};
        desc.Name = name;
        device->CreateQuery(desc, &slot.query);
        if (slot.query == nullptr) {
            supported_ = false;
        }
    }
    return supported_;
}

bool GpuPassTimer::begin(Diligent::IDeviceContext* context, std::uint64_t frame_index) {
    active_ = nullptr;
    if (!supported_) {
        return false;
    }
    bool has_new_sample = false;
    Slot& slot = slots_[frame_index % slots_.size()];
    if (slot.pending) {
        Diligent::QueryDataDuration data;
        if (slot.query->GetData(&data, sizeof(data), true)) {
            slot.pending = false;
            if (data.Frequency > 0) {
                latest_ms_ = static_cast<double>(data.Duration) * 1000.0 /
                             static_cast<double>(data.Frequency);
                has_new_sample = true;
            }
        }
    }
    if (!slot.pending) {
        context->BeginQuery(slot.query);
        active_ = &slot;
    }
    return has_new_sample;
}

void GpuPassTimer::end(Diligent::IDeviceContext* context) {
    if (active_ != nullptr) {
        context->EndQuery(active_->query);
        active_->pending = true;
        active_ = nullptr;
    }
}

} // namespace helios::examples

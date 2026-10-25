// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Small Diligent helpers shared by the full-screen lab renderers: embedded
// SPIR-V shaders, full-screen pipelines, render textures, and GPU pass timers.
#pragma once

#include "Buffer.h"
#include "DeviceContext.h"
#include "PipelineState.h"
#include "Query.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Sampler.h"
#include "Shader.h"
#include "ShaderResourceBinding.h"
#include "Texture.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace helios::examples {

// Creates a shader from an embedded offline-compiled SPIR-V module such as
// "black_hole.slang.fragment.spv". Returns null and sets `error` on failure.
[[nodiscard]] Diligent::RefCntAutoPtr<Diligent::IShader>
createEmbeddedShader(Diligent::IRenderDevice* device, char const* embedded_name, char const* label,
                     Diligent::SHADER_TYPE type, std::string& error);

enum class FullscreenBlend : std::uint8_t {
    Opaque,
    // destination + source
    Additive,
    // source + destination * (1 - source alpha): premultiplied colour over
    // what is behind it.
    PremultipliedOver,
    // source * factor + destination * (1 - factor), factor set per draw with
    // IDeviceContext::SetBlendFactors. Used for progressive accumulation.
    ConstantFactor,
};

struct FullscreenPipelineInfo {
    char const* name = "";
    Diligent::IShader* vertex_shader = nullptr;
    Diligent::IShader* pixel_shader = nullptr;
    std::span<Diligent::TEXTURE_FORMAT const> target_formats{};
    FullscreenBlend blend = FullscreenBlend::Opaque;
    // Variables that are not static (per-binding textures).
    std::span<Diligent::ShaderResourceVariableDesc const> variables{};
};

// A depth-less triangle-list pipeline for the three-vertex full-screen
// triangle of shaders/fullscreen.slang.
[[nodiscard]] Diligent::RefCntAutoPtr<Diligent::IPipelineState>
createFullscreenPipeline(Diligent::IRenderDevice* device, FullscreenPipelineInfo const& info);

// Binds a static pixel-stage variable; sets `error` when reflection does not
// find it.
[[nodiscard]] bool bindStaticVariable(Diligent::IPipelineState* pipeline, char const* name,
                                      Diligent::IDeviceObject* object, std::string& error);

// Binds a mutable pixel-stage variable of a resource binding.
[[nodiscard]] bool bindVariable(Diligent::IShaderResourceBinding* binding, char const* name,
                                Diligent::IDeviceObject* object, std::string& error);

[[nodiscard]] Diligent::RefCntAutoPtr<Diligent::IBuffer>
createDynamicUniformBuffer(Diligent::IRenderDevice* device, char const* name, std::size_t size);

// A render-target texture that can also be sampled. `sampler` (optional) is
// attached to the default shader-resource view for combined image samplers.
[[nodiscard]] Diligent::RefCntAutoPtr<Diligent::ITexture>
createRenderTexture(Diligent::IRenderDevice* device, char const* name, std::uint32_t width,
                    std::uint32_t height, Diligent::TEXTURE_FORMAT format,
                    Diligent::ISampler* sampler);

// Binds `targets`, covers them with one viewport, and draws the triangle.
void drawFullscreen(Diligent::IDeviceContext* context,
                    std::span<Diligent::ITextureView* const> targets, std::uint32_t width,
                    std::uint32_t height, Diligent::IPipelineState* pipeline,
                    Diligent::IShaderResourceBinding* resources);

// Rolling GPU duration query for one pass per frame. Results arrive a few
// frames late; `latestMs` holds the most recent one.
class GpuPassTimer {
  public:
    // Returns false when the device has no duration queries.
    bool initialize(Diligent::IRenderDevice* device, char const* name);

    // Returns true when a new measurement was read back.
    bool begin(Diligent::IDeviceContext* context, std::uint64_t frame_index);
    void end(Diligent::IDeviceContext* context);

    [[nodiscard]] bool supported() const noexcept { return supported_; }
    [[nodiscard]] double latestMs() const noexcept { return latest_ms_; }

  private:
    struct Slot {
        Diligent::RefCntAutoPtr<Diligent::IQuery> query;
        bool pending = false;
    };
    std::array<Slot, 4> slots_{};
    Slot* active_ = nullptr;
    double latest_ms_ = 0.0;
    bool supported_ = false;
};

} // namespace helios::examples

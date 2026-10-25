// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Display pipeline shared by the HDR labs: a threshold-free glare pyramid
// (shaders/hdr_glare.slang) followed by exposure, tone map and dither
// (shaders/hdr_composite.slang).  It reads a linear HDR scene texture and
// writes a display-referred image to a caller-supplied target.
#pragma once

#include "fullscreen_pass.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace helios::examples {

struct HdrDisplaySettings {
    // Linear scale applied at 0 EV. Each lab chooses what radiance its unit
    // corresponds to; the user-facing control is `exposure_ev`.
    float base_exposure = 1.0f;
    float exposure_ev = 0.0f;
    // Fraction of the image replaced by the glare pyramid, in [0, 1].
    float glare = 0.08f;
    bool tone_map = true;
    bool dither = true;
};

class HdrPostChain {
  public:
    // `output_format` is the format of the targets later passed to resolve().
    [[nodiscard]] bool initialize(Diligent::IRenderDevice* device,
                                  Diligent::IDeviceContext* context,
                                  Diligent::IShader* fullscreen_vertex_shader,
                                  Diligent::TEXTURE_FORMAT output_format, std::string& error);

    // Sampler for scene textures handed to resolve(); attach it to their
    // default shader-resource view (createRenderTexture does).
    [[nodiscard]] Diligent::ISampler* linearSampler() const noexcept { return linear_sampler_; }

    // `scene` must be a shader resource with the linear sampler attached.
    // Leaves `output` bound as the only render target.
    [[nodiscard]] bool resolve(Diligent::ITexture* scene, Diligent::ITextureView* output,
                               std::uint32_t output_width, std::uint32_t output_height,
                               HdrDisplaySettings const& settings, std::string& error);

  private:
    struct GlareLevel {
        Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
        Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> downsample; // writes this level
        Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> upsample;   // reads this level
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    [[nodiscard]] bool ensureTargets(Diligent::ITexture* scene, std::string& error);

    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device_;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> context_;
    Diligent::RefCntAutoPtr<Diligent::IShader> glare_shader_;
    Diligent::RefCntAutoPtr<Diligent::IShader> composite_shader_;
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> downsample_pipeline_;
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> upsample_pipeline_;
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> composite_pipeline_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> composite_resources_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> glare_buffer_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> composite_buffer_;
    Diligent::RefCntAutoPtr<Diligent::ISampler> linear_sampler_;

    std::vector<GlareLevel> levels_;
    // Identity of the scene texture the bindings were built for.
    Diligent::ITexture* bound_scene_ = nullptr;
    std::int32_t bound_scene_id_ = -1;
    std::uint32_t scene_width_ = 0;
    std::uint32_t scene_height_ = 0;
    std::uint64_t frame_index_ = 0;
};

} // namespace helios::examples

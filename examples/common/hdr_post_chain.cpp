// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "hdr_post_chain.hpp"

#include "shaders/abi.hpp"

#include "CommonlyUsedStates.h"
#include "MapHelper.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace helios::examples {

bool HdrPostChain::initialize(Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                              Diligent::IShader* fullscreen_vertex_shader,
                              Diligent::TEXTURE_FORMAT output_format, std::string& error) {
    if (device == nullptr || context == nullptr || fullscreen_vertex_shader == nullptr) {
        error = "HDR post chain requires a device, a context and the full-screen vertex shader";
        return false;
    }
    device_ = device;
    context_ = context;

    glare_buffer_ = createDynamicUniformBuffer(device_, "Helios HDR glare constants",
                                               sizeof(shaders::HdrGlareParams));
    composite_buffer_ = createDynamicUniformBuffer(device_, "Helios HDR composite constants",
                                                   sizeof(shaders::HdrCompositeParams));
    device_->CreateSampler(Diligent::Sam_LinearClamp, &linear_sampler_);
    glare_shader_ = createEmbeddedShader(device_, "hdr_glare.slang.fragment.spv",
                                         "Helios HDR glare pyramid shader",
                                         Diligent::SHADER_TYPE_PIXEL, error);
    composite_shader_ = createEmbeddedShader(device_, "hdr_composite.slang.fragment.spv",
                                             "Helios HDR composite shader",
                                             Diligent::SHADER_TYPE_PIXEL, error);
    if (glare_buffer_ == nullptr || composite_buffer_ == nullptr || linear_sampler_ == nullptr ||
        glare_shader_ == nullptr || composite_shader_ == nullptr) {
        if (error.empty()) {
            error = "Diligent failed to create HDR post-chain buffers or sampler";
        }
        return false;
    }

    constexpr std::array kGlareFormat{Diligent::TEX_FORMAT_RGBA16_FLOAT};
    Diligent::ShaderResourceVariableDesc const glare_variables[] = {
        {Diligent::SHADER_TYPE_PIXEL, "source", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE}};
    FullscreenPipelineInfo info;
    info.vertex_shader = fullscreen_vertex_shader;
    info.pixel_shader = glare_shader_;
    info.target_formats = kGlareFormat;
    info.variables = glare_variables;
    info.name = "Helios HDR glare downsample pipeline";
    downsample_pipeline_ = createFullscreenPipeline(device_, info);
    info.name = "Helios HDR glare upsample pipeline";
    info.blend = FullscreenBlend::Additive;
    upsample_pipeline_ = createFullscreenPipeline(device_, info);

    std::array const output_formats{output_format};
    Diligent::ShaderResourceVariableDesc const composite_variables[] = {
        {Diligent::SHADER_TYPE_PIXEL, "scene", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
        {Diligent::SHADER_TYPE_PIXEL, "glare", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE}};
    FullscreenPipelineInfo composite_info;
    composite_info.name = "Helios HDR composite pipeline";
    composite_info.vertex_shader = fullscreen_vertex_shader;
    composite_info.pixel_shader = composite_shader_;
    composite_info.target_formats = output_formats;
    composite_info.variables = composite_variables;
    composite_pipeline_ = createFullscreenPipeline(device_, composite_info);

    if (downsample_pipeline_ == nullptr || upsample_pipeline_ == nullptr ||
        composite_pipeline_ == nullptr) {
        error = "Diligent failed to create an HDR post-chain pipeline";
        return false;
    }
    return bindStaticVariable(downsample_pipeline_, "pyramid", glare_buffer_, error) &&
           bindStaticVariable(upsample_pipeline_, "pyramid", glare_buffer_, error) &&
           bindStaticVariable(composite_pipeline_, "composite", composite_buffer_, error);
}

bool HdrPostChain::ensureTargets(Diligent::ITexture* scene, std::string& error) {
    Diligent::TextureDesc const& desc = scene->GetDesc();
    if (!levels_.empty() && bound_scene_ == scene && bound_scene_id_ == scene->GetUniqueID() &&
        desc.Width == scene_width_ && desc.Height == scene_height_) {
        return true;
    }
    // Bindings hold references to the old textures; drop them first.
    composite_resources_.Release();
    levels_.clear();

    constexpr std::uint32_t kMaximumLevels = 7;
    constexpr std::uint32_t kMinimumExtent = 4;
    std::uint32_t level_width = desc.Width;
    std::uint32_t level_height = desc.Height;
    Diligent::ITexture* previous = scene;
    for (std::uint32_t index = 0; index < kMaximumLevels; ++index) {
        level_width = std::max(1U, level_width / 2U);
        level_height = std::max(1U, level_height / 2U);
        if (index > 0 && std::min(level_width, level_height) < kMinimumExtent) {
            break;
        }
        GlareLevel level;
        level.width = level_width;
        level.height = level_height;
        level.texture = createRenderTexture(device_, "Helios HDR glare level", level_width,
                                            level_height, Diligent::TEX_FORMAT_RGBA16_FLOAT,
                                            linear_sampler_);
        if (level.texture == nullptr) {
            error = "Diligent failed to create an HDR glare target";
            return false;
        }
        downsample_pipeline_->CreateShaderResourceBinding(&level.downsample, true);
        upsample_pipeline_->CreateShaderResourceBinding(&level.upsample, true);
        if (!bindVariable(level.downsample, "source",
                          previous->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE),
                          error) ||
            !bindVariable(level.upsample, "source",
                          level.texture->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE),
                          error)) {
            return false;
        }
        previous = level.texture;
        levels_.push_back(std::move(level));
    }

    composite_pipeline_->CreateShaderResourceBinding(&composite_resources_, true);
    if (levels_.empty() ||
        !bindVariable(composite_resources_, "scene",
                      scene->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE), error) ||
        !bindVariable(composite_resources_, "glare",
                      levels_.front().texture->GetDefaultView(
                          Diligent::TEXTURE_VIEW_SHADER_RESOURCE),
                      error)) {
        if (error.empty()) {
            error = "Diligent failed to create the HDR composite resource binding";
        }
        return false;
    }
    bound_scene_ = scene;
    bound_scene_id_ = scene->GetUniqueID();
    scene_width_ = desc.Width;
    scene_height_ = desc.Height;
    return true;
}

bool HdrPostChain::resolve(Diligent::ITexture* scene, Diligent::ITextureView* output,
                           std::uint32_t output_width, std::uint32_t output_height,
                           HdrDisplaySettings const& settings, std::string& error) {
    if (composite_pipeline_ == nullptr || scene == nullptr || output == nullptr ||
        output_width == 0 || output_height == 0) {
        error = "HDR post chain was not initialized or received an empty scene or output";
        return false;
    }
    if (!ensureTargets(scene, error)) {
        return false;
    }

    auto writeGlare = [&](std::uint32_t source_width, std::uint32_t source_height, float mode) {
        Diligent::MapHelper<shaders::HdrGlareParams> mapped{context_, glare_buffer_,
                                                           Diligent::MAP_WRITE,
                                                           Diligent::MAP_FLAG_DISCARD};
        mapped->params[0] = 1.0f / static_cast<float>(source_width);
        mapped->params[1] = 1.0f / static_cast<float>(source_height);
        mapped->params[2] = mode;
        mapped->params[3] = 1.0f;
    };

    // Downsample chain: scene -> level 0 -> level 1 -> ...
    std::uint32_t source_width = scene_width_;
    std::uint32_t source_height = scene_height_;
    for (std::size_t index = 0; index < levels_.size(); ++index) {
        GlareLevel& level = levels_[index];
        writeGlare(source_width, source_height, index == 0 ? 0.0f : 1.0f);
        Diligent::ITextureView* const target =
            level.texture->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET);
        drawFullscreen(context_, {&target, 1}, level.width, level.height, downsample_pipeline_,
                       level.downsample);
        source_width = level.width;
        source_height = level.height;
    }
    // Upsample chain: each level is added into the next finer one.
    for (std::size_t index = levels_.size() - 1; index > 0; --index) {
        GlareLevel& coarse = levels_[index];
        GlareLevel& fine = levels_[index - 1];
        writeGlare(coarse.width, coarse.height, 2.0f);
        Diligent::ITextureView* const target =
            fine.texture->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET);
        drawFullscreen(context_, {&target, 1}, fine.width, fine.height, upsample_pipeline_,
                       coarse.upsample);
    }

    {
        Diligent::MapHelper<shaders::HdrCompositeParams> mapped{context_, composite_buffer_,
                                                               Diligent::MAP_WRITE,
                                                               Diligent::MAP_FLAG_DISCARD};
        mapped->exposure[0] = settings.base_exposure * std::exp2(settings.exposure_ev);
        mapped->exposure[1] = std::clamp(settings.glare, 0.0f, 1.0f);
        mapped->exposure[2] = 1.0f / static_cast<float>(levels_.size());
        mapped->exposure[3] = settings.tone_map ? 0.0f : 1.0f;
        mapped->dither[0] = static_cast<float>(frame_index_ % 1024U);
        mapped->dither[1] = settings.dither ? 1.0f : 0.0f;
        mapped->dither[2] = 0.0f;
        mapped->dither[3] = 0.0f;
    }
    drawFullscreen(context_, {&output, 1}, output_width, output_height, composite_pipeline_,
                   composite_resources_);
    ++frame_index_;
    return true;
}

} // namespace helios::examples

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "galaxy_scene.hpp"

#include "deep_sky_renderer.hpp"

#include "MapHelper.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace helios::examples {
namespace {

constexpr double kPopulationMinimumMyr = 0.5;
constexpr double kPopulationMaximumMyr = 14000.0;
constexpr std::uint32_t kPopulationSamples = 256;
constexpr double kPi = std::numbers::pi;

// Radiance written to the HDR target per solar luminosity per square parsec.
// A disk like the Sun's neighbourhood (about 25 solar luminosities per pc^2
// seen face-on) lands in the lower part of the display range, leaving room
// for the bulge, a hundred times brighter.
constexpr float kLightScale = 0.006f;
// Visual optical depth of one solar mass of gas per pc^2 with the Milky
// Way's dust: N_H = 8.9e19 cm^-2 (71 % hydrogen), A_V = N_H / 1.9e21 cm^-2
// (Bohlin, Savage & Drake 1978), tau = 0.921 A_V.
constexpr float kDustOpacity = 0.043f;
constexpr float kSmallestSigmaPixels = 1.0f;
// Charlot & Fall (2000), as in galaxy_stars.slang.
constexpr double kBirthCloudDepth = 1.0;
constexpr double kBirthCloudMyr = 10.0;

// Bytes of a new state uploaded per frame: about a millisecond of copying.
constexpr std::size_t kUploadBytesPerFrame = 8U << 20U;
// How far the simulation is kept ahead of the display clock, and how far the
// display may run past the newest state on the particles' velocities alone.
constexpr double kSimulationLeadMyr = 1.0;
constexpr double kExtrapolationMyr = 1.0;
// kpc travelled per Myr at 1 km/s.
constexpr double kKpcPerMyrPerKmS = 1.0227e-3;
// Longer than the shader keeps a population in its young-star and ionised-gas
// passes (YOUNG_MYR and IONISING_MYR in shaders/galaxy_stars.slang).
constexpr double kYoungestInitialMyr = 70.0;

constexpr double kSecondsPerMyr = 3.15576e13;
// The fine part of the display clock is folded into the coarse part past this.
constexpr double kFineFoldSeconds = 1.0e9;
constexpr double kKmPerPc = 3.0856776e13;
constexpr double kKmPerKpc = 3.0856776e16;
constexpr double kKmPerAu = 1.495978707e8;
constexpr double kKmPerSolarRadius = 695700.0;
constexpr double kKmPerEarthRadius = 6371.0;
constexpr double kPcPerSolarRadius = kKmPerSolarRadius / kKmPerPc;

// Single stars: the width of a star's image (px), and how bright its image
// must peak to be drawn, as a fraction of display white.
constexpr float kStarSigmaPixels = 0.8f;
constexpr double kVisibleRadiance = 0.01;
// Stars are resolved in particles at most this far, and in at most this
// many particles (the nearest).
constexpr double kMaxReachKpc = 3.0;
constexpr std::size_t kMaxNearCandidates = 60000;
constexpr std::uint32_t kMaxSlotsPerParticle = 65536;
// An unbound young association expands at about 3 km/s; it starts a few
// parsecs across (galaxy_stars.slang).
constexpr double kClusterRadiusPc = 4.0;
constexpr double kDispersalPcPerMyr = 3.07;
// The visited system is drawn as spheres within this distance of its star;
// beyond, its star is one of the single stars.
constexpr double kSystemReachPc = 1.0;

void set(float destination[4], double x, double y, double z, double w) {
    destination[0] = static_cast<float>(x);
    destination[1] = static_cast<float>(y);
    destination[2] = static_cast<float>(z);
    destination[3] = static_cast<float>(w);
}

void store(float destination[4], math::Vec3d const& value, double w) {
    set(destination, value[0], value[1], value[2], w);
}

[[nodiscard]] double smoothstep(double edge0, double edge1, double x) {
    double const t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

[[nodiscard]] Diligent::RefCntAutoPtr<Diligent::IBuffer>
createStructuredBuffer(Diligent::IRenderDevice* device, char const* name, std::size_t elements,
                       std::uint32_t stride) {
    Diligent::BufferDesc desc;
    desc.Name = name;
    desc.Size = static_cast<Diligent::Uint64>(std::max<std::size_t>(elements, 1U)) * stride;
    desc.BindFlags = Diligent::BIND_SHADER_RESOURCE;
    desc.Usage = Diligent::USAGE_DEFAULT;
    desc.Mode = Diligent::BUFFER_MODE_STRUCTURED;
    desc.ElementByteStride = stride;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> buffer;
    device->CreateBuffer(desc, nullptr, &buffer);
    return buffer;
}

} // namespace

bool GalaxyScene::initialize(Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                             Diligent::IBuffer* frame_buffer, Diligent::TEXTURE_FORMAT target_format,
                             Diligent::ITextureView* blackbody_view, blackhole::BlackbodyTable const& blackbody,
                             std::string& error) {
    device_ = device;
    context_ = context;
    frame_buffer_ = frame_buffer;
    blackbody_view_ = blackbody_view;
    blackbody_ = blackbody;
    solar_log_luminance_ = blackhole::sampleBlackbodyLogLuminance(blackbody_, 5772.0);

    population_ =
        makePopulationTable(kPopulationMinimumMyr, kPopulationMaximumMyr, kPopulationSamples);
    ionised_light_ = ionisedGasLight();
    Diligent::TextureDesc table_desc;
    table_desc.Name = "Helios stellar population table";
    table_desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
    table_desc.Width = population_.count;
    table_desc.Height = 1;
    table_desc.MipLevels = 1;
    table_desc.Format = Diligent::TEX_FORMAT_RGBA32_FLOAT;
    table_desc.Usage = Diligent::USAGE_IMMUTABLE;
    table_desc.BindFlags = Diligent::BIND_SHADER_RESOURCE;
    Diligent::TextureSubResData table_level;
    table_level.pData = population_.rgba.data();
    table_level.Stride = static_cast<Diligent::Uint64>(population_.count) * 4U * sizeof(float);
    Diligent::TextureData table_data;
    table_data.pSubResources = &table_level;
    table_data.NumSubresources = 1;
    device_->CreateTexture(table_desc, &table_data, &population_texture_);

    Diligent::SamplerDesc sampler;
    sampler.Name = "Helios galaxy map sampler";
    sampler.MinFilter = sampler.MagFilter = Diligent::FILTER_TYPE_LINEAR;
    sampler.MipFilter = Diligent::FILTER_TYPE_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = Diligent::TEXTURE_ADDRESS_CLAMP;
    device_->CreateSampler(sampler, &sampler_);
    if (population_texture_ == nullptr || sampler_ == nullptr || blackbody_view_ == nullptr) {
        error = "Diligent failed to create the galaxy lookup resources";
        return false;
    }

    auto vertex_shader =
        createEmbeddedShader(device_, "galaxy_stars.slang.vertex.spv", "Helios galaxy star vertex shader",
                             Diligent::SHADER_TYPE_VERTEX, error);
    auto pixel_shader =
        createEmbeddedShader(device_, "galaxy_stars.slang.fragment.spv",
                             "Helios galaxy star fragment shader", Diligent::SHADER_TYPE_PIXEL, error);
    if (vertex_shader == nullptr || pixel_shader == nullptr) {
        return false;
    }

    Diligent::GraphicsPipelineStateCreateInfo info;
    info.PSODesc.Name = "Helios deep-sky galaxy star pipeline";
    info.PSODesc.PipelineType = Diligent::PIPELINE_TYPE_GRAPHICS;
    info.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
    Diligent::ShaderResourceVariableDesc const variables[] = {
        {Diligent::SHADER_TYPE_VERTEX, "galaxy_maps", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE}};
    info.PSODesc.ResourceLayout.Variables = variables;
    info.PSODesc.ResourceLayout.NumVariables = 1;
    auto& graphics = info.GraphicsPipeline;
    graphics.NumRenderTargets = 1;
    graphics.RTVFormats[0] = target_format;
    graphics.DSVFormat = Diligent::TEX_FORMAT_UNKNOWN;
    graphics.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    graphics.RasterizerDesc.CullMode = Diligent::CULL_MODE_NONE;
    graphics.DepthStencilDesc.DepthEnable = false;
    graphics.DepthStencilDesc.DepthWriteEnable = false;
    // Light adds.
    auto& blend = graphics.BlendDesc.RenderTargets[0];
    blend.BlendEnable = true;
    blend.SrcBlend = Diligent::BLEND_FACTOR_ONE;
    blend.DestBlend = Diligent::BLEND_FACTOR_ONE;
    blend.BlendOp = Diligent::BLEND_OPERATION_ADD;
    blend.SrcBlendAlpha = Diligent::BLEND_FACTOR_ZERO;
    blend.DestBlendAlpha = Diligent::BLEND_FACTOR_ONE;
    blend.BlendOpAlpha = Diligent::BLEND_OPERATION_ADD;
    // One shaders::GalaxyParticle per instance: position and birth time as
    // normalised uint16, velocity as normalised int16; and from a second
    // buffer the fraction of its light not drawn as single stars.
    Diligent::LayoutElement const elements[] = {
        {0, 0, 4, Diligent::VT_UINT16, true, Diligent::INPUT_ELEMENT_FREQUENCY_PER_INSTANCE},
        {1, 0, 4, Diligent::VT_INT16, true, Diligent::INPUT_ELEMENT_FREQUENCY_PER_INSTANCE},
        {2, 1, 1, Diligent::VT_UINT8, true, Diligent::INPUT_ELEMENT_FREQUENCY_PER_INSTANCE}};
    graphics.InputLayout.LayoutElements = elements;
    graphics.InputLayout.NumElements = 3;
    info.pVS = vertex_shader;
    info.pPS = pixel_shader;
    device_->CreateGraphicsPipelineState(info, &pipeline_);
    if (pipeline_ == nullptr) {
        error = "Diligent failed to create the galaxy star pipeline";
        return false;
    }
    auto* const frame_variable =
        pipeline_->GetStaticVariableByName(Diligent::SHADER_TYPE_VERTEX, "frame");
    auto* const table_variable =
        pipeline_->GetStaticVariableByName(Diligent::SHADER_TYPE_VERTEX, "population_table");
    if (frame_variable == nullptr || table_variable == nullptr) {
        error = "Diligent could not reflect the galaxy star shader variables";
        return false;
    }
    frame_variable->Set(frame_buffer_);
    table_variable->Set(population_texture_->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
    return createStarPipelines(target_format, error);
}

bool GalaxyScene::createStarPipelines(Diligent::TEXTURE_FORMAT target_format, std::string& error) {
    // Single stars: instanced quads without vertex buffers.
    auto vertex_shader = createEmbeddedShader(device_, "galaxy_resolved_stars.slang.vertex.spv",
                                              "Helios galaxy single-star vertex shader",
                                              Diligent::SHADER_TYPE_VERTEX, error);
    auto pixel_shader = createEmbeddedShader(device_, "galaxy_resolved_stars.slang.fragment.spv",
                                             "Helios galaxy single-star fragment shader",
                                             Diligent::SHADER_TYPE_PIXEL, error);
    if (vertex_shader == nullptr || pixel_shader == nullptr) {
        return false;
    }
    Diligent::GraphicsPipelineStateCreateInfo info;
    info.PSODesc.Name = "Helios deep-sky galaxy single-star pipeline";
    info.PSODesc.PipelineType = Diligent::PIPELINE_TYPE_GRAPHICS;
    info.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
    Diligent::ShaderResourceVariableDesc const variables[] = {
        {Diligent::SHADER_TYPE_VERTEX, "near_particles", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
        {Diligent::SHADER_TYPE_VERTEX, "star_chunks", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE}};
    info.PSODesc.ResourceLayout.Variables = variables;
    info.PSODesc.ResourceLayout.NumVariables = 2;
    auto& graphics = info.GraphicsPipeline;
    graphics.NumRenderTargets = 1;
    graphics.RTVFormats[0] = target_format;
    graphics.DSVFormat = Diligent::TEX_FORMAT_UNKNOWN;
    graphics.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    graphics.RasterizerDesc.CullMode = Diligent::CULL_MODE_NONE;
    graphics.DepthStencilDesc.DepthEnable = false;
    graphics.DepthStencilDesc.DepthWriteEnable = false;
    auto& blend = graphics.BlendDesc.RenderTargets[0];
    blend.BlendEnable = true;
    blend.SrcBlend = Diligent::BLEND_FACTOR_ONE;
    blend.DestBlend = Diligent::BLEND_FACTOR_ONE;
    blend.BlendOp = Diligent::BLEND_OPERATION_ADD;
    blend.SrcBlendAlpha = Diligent::BLEND_FACTOR_ZERO;
    blend.DestBlendAlpha = Diligent::BLEND_FACTOR_ONE;
    blend.BlendOpAlpha = Diligent::BLEND_OPERATION_ADD;
    info.pVS = vertex_shader;
    info.pPS = pixel_shader;
    device_->CreateGraphicsPipelineState(info, &star_pipeline_);
    if (star_pipeline_ == nullptr) {
        error = "Diligent failed to create the galaxy single-star pipeline";
        return false;
    }
    auto* const frame_variable = star_pipeline_->GetStaticVariableByName(Diligent::SHADER_TYPE_VERTEX, "frame");
    auto* const table_variable =
        star_pipeline_->GetStaticVariableByName(Diligent::SHADER_TYPE_VERTEX, "blackbody_table");
    if (frame_variable == nullptr || table_variable == nullptr) {
        error = "Diligent could not reflect the galaxy single-star shader variables";
        return false;
    }
    frame_variable->Set(frame_buffer_);
    table_variable->Set(blackbody_view_);

    // The visited system: a full-screen pass over the galaxy.
    auto fullscreen = createEmbeddedShader(device_, "fullscreen.slang.vertex.spv", "Helios full-screen vertex shader",
                                           Diligent::SHADER_TYPE_VERTEX, error);
    auto system_shader = createEmbeddedShader(device_, "star_system.slang.fragment.spv",
                                              "Helios star-system fragment shader", Diligent::SHADER_TYPE_PIXEL, error);
    if (fullscreen == nullptr || system_shader == nullptr) {
        return false;
    }
    Diligent::TEXTURE_FORMAT const formats[] = {target_format};
    FullscreenPipelineInfo system_info;
    system_info.name = "Helios deep-sky star-system pipeline";
    system_info.vertex_shader = fullscreen;
    system_info.pixel_shader = system_shader;
    system_info.target_formats = formats;
    system_info.blend = FullscreenBlend::PremultipliedOver;
    system_pipeline_ = createFullscreenPipeline(device_, system_info);
    system_buffer_ = createDynamicUniformBuffer(device_, "Helios star-system constants", sizeof(shaders::StarSystemBlock));
    if (system_pipeline_ == nullptr || system_buffer_ == nullptr ||
        !bindStaticVariable(system_pipeline_, "frame", frame_buffer_, error) ||
        !bindStaticVariable(system_pipeline_, "blackbody_table", blackbody_view_, error) ||
        !bindStaticVariable(system_pipeline_, "system", system_buffer_, error)) {
        if (error.empty()) {
            error = "Diligent failed to create the star-system pipeline";
        }
        return false;
    }
    system_pipeline_->CreateShaderResourceBinding(&system_resources_, true);
    if (system_resources_ == nullptr) {
        error = "Diligent failed to create the star-system resource binding";
        return false;
    }
    return true;
}

double GalaxyScene::displayMyr() const noexcept {
    return display_time_myr_ + display_fine_s_ / kSecondsPerMyr;
}

double GalaxyScene::displayCoarseSeconds() const noexcept {
    return display_time_myr_ * kSecondsPerMyr;
}

bool GalaxyScene::update(GalaxySettings const& settings, std::uint32_t threads, bool wait,
                         bool& changed, std::string& error) {
    using Clock = std::chrono::steady_clock;
    changed = false;
    Clock::time_point const now = Clock::now();
    double const elapsed = has_front_ || runner_ != nullptr
                               ? std::clamp(std::chrono::duration<double>(now - last_update_).count(),
                                            0.0, 0.1)
                               : 0.0;
    last_update_ = now;

    if (runner_ == nullptr || !(runner_->recipe() == settings.recipe) ||
        generation_ != settings.generation) {
        GalaxyRecipe const& recipe = settings.recipe;
        if (recipe.grid < 64 || recipe.grid > 2048 || (recipe.grid & (recipe.grid - 1U)) != 0 ||
            recipe.disk_stars == 0 || recipe.disk_stars + recipe.bulge_stars + recipe.gas_clouds > 16'000'000U) {
            error = "the galaxy mesh must be a power of two in [64, 2048] and hold at most 16 million "
                    "particles";
            return false;
        }
        // The previous run's thread must stop before its replacement starts.
        runner_.reset();
        runner_ = std::make_unique<GalaxyRunner>(recipe, threads);
        generation_ = settings.generation;
        version_ = 0;
        uploading_ = false;
        has_front_ = false;
        display_time_myr_ = 0.0;
        display_fine_s_ = 0.0;
        rate_scale_ = 1.0;
        status_ = {};
        system_.reset();
        near_.clear();
        star_instances_ = 0;
        // Every particle's image has all its light again (uploaded with the
        // next view).
        std::fill(unresolved_.begin(), unresolved_.end(), static_cast<std::uint8_t>(255U));
        unresolved_set_.clear();
        unresolved_dirty_ = true;
        changed = true;
    }
    double const stop = std::max(settings.time_myr, 0.0);
    last_elapsed_ = elapsed;
    adapt_at_once_ = wait;

    if (wait) {
        // A capture: simulate to the stop time and show exactly that state.
        runner_->setTargetMyr(stop);
        runner_->waitForTarget();
        if (runner_->fetch(incoming_, version_)) {
            uploading_ = true;
            uploaded_bytes_ = 0;
        }
        bool finished = false;
        if (uploading_ && !continueUpload(static_cast<std::size_t>(-1), finished, error)) {
            return false;
        }
        changed = changed || finished || displayMyr() != front_time_myr_;
        display_time_myr_ = front_time_myr_;
        display_fine_s_ = 0.0;
    } else {
        // The display clock: forwards with wall time, never past what the
        // particles' velocities can be trusted for.  Small steps go to the
        // fine part, so that a system's planets keep moving when the galaxy
        // is shown hours per second.
        double const before = displayMyr();
        double const newest = status_.time_myr;
        if (settings.speed_myr_per_second > 0.0 && has_front_) {
            double const step_myr = rate_scale_ * settings.speed_myr_per_second * elapsed;
            if (step_myr * kSecondsPerMyr < kFineFoldSeconds) {
                display_fine_s_ += step_myr * kSecondsPerMyr;
                if (display_fine_s_ > kFineFoldSeconds) {
                    display_time_myr_ += display_fine_s_ / kSecondsPerMyr;
                    display_fine_s_ = 0.0;
                }
            } else {
                display_time_myr_ += step_myr + display_fine_s_ / kSecondsPerMyr;
                display_fine_s_ = 0.0;
            }
            // Never further than the velocities can be trusted past the
            // newest state; at the stop time the planets of a visited system
            // may still run on for as long.
            double const limit = std::min(stop, newest) + kExtrapolationMyr;
            if (displayMyr() > limit) {
                display_time_myr_ = std::max(limit, before);
                display_fine_s_ = 0.0;
                if (newest < stop) {
                    // The simulation cannot keep this pace: ease off.
                    rate_scale_ = std::max(rate_scale_ * 0.97, 0.02);
                }
            } else if (displayMyr() < newest) {
                rate_scale_ = std::min(rate_scale_ * 1.02, 1.0);
            }
        }
        runner_->setTargetMyr(std::min(stop, displayMyr() + kSimulationLeadMyr));

        if (!uploading_ && runner_->fetch(incoming_, version_)) {
            uploading_ = true;
            uploaded_bytes_ = 0;
            status_.time_myr = incoming_.time_myr;
            status_.steps = incoming_.step;
            status_.stars_formed = incoming_.stars_formed;
            status_.clouds = incoming_.clouds;
            status_.star_formation_rate = incoming_.star_formation_rate;
            status_.step_ms = incoming_.step_ms;
        }
        bool finished = false;
        if (uploading_ && !continueUpload(kUploadBytesPerFrame, finished, error)) {
            return false;
        }
        changed = changed || finished || displayMyr() != before;
    }
    status_.display_time_myr = displayMyr();
    status_.initialise_seconds = runner_->initialiseSeconds();
    status_.ready = has_front_;
    return true;
}

bool GalaxyScene::ensureBuffers(std::string& error) {
    auto const particle_count = static_cast<std::uint32_t>(incoming_.particles.size());
    if (particle_count != buffer_particles_) {
        for (auto& buffer : particles_) {
            buffer.Release();
            Diligent::BufferDesc desc;
            desc.Name = "Helios galaxy star particles";
            desc.Size = static_cast<Diligent::Uint64>(particle_count) * sizeof(shaders::GalaxyParticle);
            desc.BindFlags = Diligent::BIND_VERTEX_BUFFER;
            desc.Usage = Diligent::USAGE_DEFAULT;
            device_->CreateBuffer(desc, nullptr, &buffer);
            if (buffer == nullptr) {
                error = "Diligent failed to create the galaxy particle buffers";
                return false;
            }
        }
        unresolved_buffer_.Release();
        Diligent::BufferDesc desc;
        desc.Name = "Helios galaxy unresolved light";
        desc.Size = std::max<Diligent::Uint64>(particle_count, 4U);
        desc.BindFlags = Diligent::BIND_VERTEX_BUFFER;
        desc.Usage = Diligent::USAGE_DEFAULT;
        unresolved_.assign(particle_count, 255U);
        unresolved_set_.clear();
        Diligent::BufferData data{unresolved_.data(), static_cast<Diligent::Uint64>(unresolved_.size())};
        device_->CreateBuffer(desc, particle_count > 0 ? &data : nullptr, &unresolved_buffer_);
        if (unresolved_buffer_ == nullptr) {
            error = "Diligent failed to create the galaxy unresolved-light buffer";
            return false;
        }
        unresolved_dirty_ = false;
        buffer_particles_ = particle_count;
        has_front_ = false;
    }
    if (maps_texture_ == nullptr || incoming_.grid != maps_grid_) {
        resources_.Release();
        maps_texture_.Release();
        Diligent::TextureDesc desc;
        desc.Name = "Helios galaxy surface-density maps";
        desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
        desc.Width = incoming_.grid;
        desc.Height = incoming_.grid;
        desc.MipLevels = 1;
        desc.Format = Diligent::TEX_FORMAT_RGBA32_FLOAT;
        desc.Usage = Diligent::USAGE_DEFAULT;
        desc.BindFlags = Diligent::BIND_SHADER_RESOURCE;
        device_->CreateTexture(desc, nullptr, &maps_texture_);
        if (maps_texture_ == nullptr) {
            error = "Diligent failed to create the galaxy maps";
            return false;
        }
        auto* const view = maps_texture_->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
        view->SetSampler(sampler_);
        pipeline_->CreateShaderResourceBinding(&resources_, true);
        auto* const variable = resources_ != nullptr
                                   ? resources_->GetVariableByName(Diligent::SHADER_TYPE_VERTEX, "galaxy_maps")
                                   : nullptr;
        if (variable == nullptr) {
            error = "Diligent could not reflect the galaxy map variable";
            return false;
        }
        variable->Set(view);
        maps_grid_ = incoming_.grid;
    }
    return true;
}

bool GalaxyScene::continueUpload(std::size_t budget, bool& finished, std::string& error) {
    finished = false;
    if (!ensureBuffers(error)) {
        return false;
    }
    std::size_t const back = 1U - front_;
    std::size_t const total = incoming_.particles.size() * sizeof(shaders::GalaxyParticle);
    std::size_t const bytes = std::min(budget, total - uploaded_bytes_);
    if (bytes > 0) {
        context_->UpdateBuffer(particles_[back], uploaded_bytes_, bytes,
                               reinterpret_cast<std::uint8_t const*>(incoming_.particles.data()) +
                                   uploaded_bytes_,
                               Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        uploaded_bytes_ += bytes;
    }
    if (uploaded_bytes_ < total) {
        return true;
    }

    // Complete: the maps go with it, and it becomes the drawn state.
    Diligent::Box const box{0, incoming_.grid, 0, incoming_.grid};
    Diligent::TextureSubResData data;
    data.pData = incoming_.maps.data();
    data.Stride = static_cast<Diligent::Uint64>(incoming_.grid) * 4U * sizeof(float);
    context_->UpdateTexture(maps_texture_, 0, 0, box, data,
                            Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                            Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    front_ = back;
    has_front_ = true;
    uploading_ = false;
    // The drawn state stays on the CPU for the single stars and for picking;
    // the previous one goes back to the simulation as storage.
    std::swap(front_state_, incoming_);
    front_time_myr_ = front_state_.time_myr;
    front_particles_ = static_cast<std::uint32_t>(front_state_.particles.size());
    disk_stars_ = front_state_.disk_stars;
    bulge_stars_ = front_state_.bulge_stars;
    status_.time_myr = std::max(status_.time_myr, front_state_.time_myr);
    status_.steps = front_state_.step;
    status_.particles = front_particles_;
    status_.stars_formed = front_state_.stars_formed;
    status_.clouds = front_state_.clouds;
    status_.star_formation_rate = front_state_.star_formation_rate;
    status_.step_ms = front_state_.step_ms;
    finished = true;
    return true;
}

// ---- particles of the drawn state ---------------------------------------------------

math::Vec3d GalaxyScene::particlePosition(std::uint32_t index) const {
    GalaxyParticleState const state = unpackParticle(front_state_.particles[index]);
    double const drift = std::clamp(displayMyr() - front_time_myr_, -5.0, 5.0) * kKpcPerMyrPerKmS;
    return {state.position_kpc[0] + state.velocity_km_s[0] * drift,
            state.position_kpc[1] + state.velocity_km_s[1] * drift,
            state.position_kpc[2] + state.velocity_km_s[2] * drift};
}

double GalaxyScene::particleMass(std::uint32_t index) const {
    if (index < disk_stars_) {
        return runner_->diskParticleMass();
    }
    return index < disk_stars_ + bulge_stars_ ? runner_->bulgeParticleMass() : runner_->cloudParticleMass();
}

double GalaxyScene::particleBirth(std::uint32_t index) const {
    return unpackParticle(front_state_.particles[index]).birth_myr;
}

bool GalaxyScene::isBulge(std::uint32_t index) const noexcept {
    return index >= disk_stars_ && index < disk_stars_ + bulge_stars_;
}

// The stars of a particle, spread over the distance its own image covers:
// the mean spacing of particles in the disk's mass model (or the bulge's),
// a few parsecs growing with age for a young association.  A smooth
// function of where the particle is, so that its stars do not move when a
// new state of the simulation arrives.
ParticleStars GalaxyScene::starsOf(std::uint32_t index, math::Vec3d const& position) const {
    GalaxyRecipe const& recipe = runner_->recipe();
    double const mass = particleMass(index);
    double const birth = particleBirth(index);
    double const age = std::max(displayMyr() - birth, 0.0);
    bool const bulge = isBulge(index);
    double const smoothing = 3.5;
    double spread_kpc = 0.0;
    double thickness_kpc = 0.0;
    if (bulge) {
        double const scale = recipe.bulge_scale_kpc;
        double const radius = std::max(math::length(position), 0.02 * scale);
        double const sum = radius + scale;
        double const density = recipe.bulge_mass * 1.0e10 * scale / (2.0 * kPi * radius * sum * sum * sum);
        spread_kpc = smoothing * 0.6 * std::cbrt(mass / density);
        thickness_kpc = spread_kpc;
    } else {
        double const radius = std::hypot(position[0], position[1]);
        double const disk = recipe.disk_mass * 1.0e10 / (2.0 * kPi * recipe.disk_scale_kpc * recipe.disk_scale_kpc) *
                            std::exp(-radius / recipe.disk_scale_kpc);
        double const gas = recipe.gas_mass * 1.0e10 / (2.0 * kPi * recipe.gas_scale_kpc * recipe.gas_scale_kpc) *
                           std::exp(-radius / recipe.gas_scale_kpc);
        spread_kpc = smoothing * std::sqrt(mass / std::max(disk + gas, 1.0e6));
        spread_kpc = std::min(spread_kpc, (kClusterRadiusPc + kDispersalPcPerMyr * age) * 1.0e-3);
        thickness_kpc = std::min(spread_kpc, 0.15);
    }
    spread_kpc = std::clamp(spread_kpc, 1.0e-3, 2.0);
    thickness_kpc = std::clamp(thickness_kpc, 1.0e-3, 2.0);
    return particleStars(index, mass, age, birth, bulge, spread_kpc * 1000.0, thickness_kpc * 1000.0);
}

// Gas column (solar masses per pc^2) between a point and the camera, through
// the simulated gas layer: the CPU twin of gasColumn in galaxy_stars.slang.
float GalaxyScene::gasColumn(math::Vec3d const& position, math::Vec3d const& to_camera) const {
    std::uint32_t const n = front_state_.grid;
    if (n == 0 || front_state_.maps.size() < static_cast<std::size_t>(n) * n * 4U) {
        return 0.0f;
    }
    double const half = front_state_.half_extent_kpc;
    auto mapsAt = [&](double x, double y) {
        double const u = std::clamp((x * (0.5 / half) + 0.5) * n - 0.5, 0.0, n - 1.001);
        double const v = std::clamp((y * (0.5 / half) + 0.5) * n - 0.5, 0.0, n - 1.001);
        auto const i = static_cast<std::size_t>(u);
        auto const j = static_cast<std::size_t>(v);
        double const tu = u - static_cast<double>(i);
        double const tv = v - static_cast<double>(j);
        std::array<double, 2> result{};
        for (std::size_t c = 0; c < 2; ++c) {
            auto at = [&](std::size_t a, std::size_t b) {
                return static_cast<double>(front_state_.maps[(b * n + a) * 4U + c]);
            };
            result[c] = (at(i, j) * (1.0 - tu) + at(i + 1, j) * tu) * (1.0 - tv) +
                        (at(i, j + 1) * (1.0 - tu) + at(i + 1, j + 1) * tu) * tv;
        }
        return result;
    };
    constexpr int kSamples = 6;
    constexpr double kMaxPath = 12.0;
    double const thickness = std::max(mapsAt(position[0], position[1])[1], 0.02);
    double const direction = to_camera[2] >= 0.0 ? 1.0 : -1.0;
    double const slope = direction * std::max(std::abs(to_camera[2]), 0.02);
    double const exit_height = direction * 3.0 * thickness;
    if ((position[2] - exit_height) * direction >= 0.0) {
        return 0.0f;
    }
    double const entry_height = (position[2] + exit_height) * direction < 0.0 ? -exit_height : position[2];
    double climb = exit_height - entry_height;
    climb = direction * std::min(std::abs(climb), kMaxPath * std::abs(slope));
    double column = 0.0;
    for (int index = 0; index < kSamples; ++index) {
        double const height = entry_height + climb * ((index + 0.5) / kSamples);
        double const along = (height - position[2]) / slope;
        auto const gas = mapsAt(position[0] + to_camera[0] * along, position[1] + to_camera[1] * along);
        double const g = std::max(gas[1], 0.02);
        double const scaled = height / g;
        column += gas[0] * std::exp(-0.5 * scaled * scaled) / (2.5066283 * g);
    }
    return static_cast<float>(column * std::abs(climb) / (kSamples * std::abs(slope)));
}

std::optional<math::Vec3d> GalaxyScene::focusPosition(GalaxyFocus const& focus) const {
    if (focus.kind == GalaxyFocus::Kind::Centre) {
        return math::Vec3d{0.0, 0.0, 0.0};
    }
    if (!has_front_ || runner_ == nullptr || focus.particle >= front_state_.particles.size()) {
        return std::nullopt;
    }
    math::Vec3d const particle = particlePosition(focus.particle);
    if (focus.kind == GalaxyFocus::Kind::Particle) {
        return particle;
    }
    ParticleStars const stars = starsOf(focus.particle, particle);
    if (focus.rank >= stars.count) {
        return std::nullopt;
    }
    math::Vec3d const star = particle + starOffsetPc(stars, focus.rank) * 1.0e-3;
    if (focus.kind == GalaxyFocus::Kind::Star) {
        return star;
    }
    StarSystem const generated =
        system_ && system_->key == StarKey{focus.particle, focus.rank} ? *system_ : generateStarSystem(stars, focus.rank);
    if (focus.planet >= generated.planet_count) {
        return star;
    }
    Planet const& planet = generated.planets[focus.planet];
    auto const& axes = generated.axes;
    math::Vec3d const local = planetPosition(planet, displayCoarseSeconds(), display_fine_s_) * (kKmPerAu / kKmPerKpc);
    math::Vec3d const planet_world = star + axes[0] * local[0] + axes[1] * local[1] + axes[2] * local[2];
    if (focus.kind == GalaxyFocus::Kind::Planet || focus.moon >= planet.moon_count) {
        return planet_world;
    }
    math::Vec3d const moon = moonPosition(planet.moons[focus.moon], displayCoarseSeconds(), display_fine_s_) / kKmPerKpc;
    math::Vec3d const around = planet.equator[0] * moon[0] + planet.equator[1] * moon[1] + planet.equator[2] * moon[2];
    return planet_world + axes[0] * around[0] + axes[1] * around[1] + axes[2] * around[2];
}

double GalaxyScene::focusRadius(GalaxyFocus const& focus) const {
    if (!system_ || !focus.hasStar() || system_->key != StarKey{focus.particle, focus.rank}) {
        return 0.0;
    }
    StarSystem const& system = *system_;
    if (focus.kind == GalaxyFocus::Kind::Star || focus.planet >= system.planet_count) {
        return system.star.radius * kKmPerSolarRadius / kKmPerKpc;
    }
    Planet const& planet = system.planets[focus.planet];
    if (focus.kind == GalaxyFocus::Kind::Planet || focus.moon >= planet.moon_count) {
        return planet.radius_earth * kKmPerEarthRadius / kKmPerKpc;
    }
    return planet.moons[focus.moon].radius_km / kKmPerKpc;
}

std::optional<math::Vec3d> GalaxyScene::planetWorld(std::uint32_t planet, double offset_s) const {
    if (!system_ || planet >= system_->planet_count || bodies_.empty()) {
        return std::nullopt;
    }
    math::Vec3d const& star = bodies_.front().world_kpc;
    auto const& axes = system_->axes;
    math::Vec3d const local = planetPosition(system_->planets[planet], displayCoarseSeconds(), display_fine_s_ + offset_s) *
                              (kKmPerAu / kKmPerKpc);
    return star + axes[0] * local[0] + axes[1] * local[1] + axes[2] * local[2];
}

bool GalaxyScene::project(math::Vec3d const& world_kpc, double& x, double& y) const {
    math::Vec3d const relative = world_kpc - camera_world_;
    double const depth = math::dot(relative, camera_forward_);
    if (depth <= 0.0) {
        return false;
    }
    x = math::dot(relative, camera_right_) / (depth * math::dot(camera_right_, camera_right_));
    y = math::dot(relative, camera_up_) / (depth * math::dot(camera_up_, camera_up_));
    return true;
}

// ---- the view -------------------------------------------------------------------------

bool GalaxyScene::prepareView(DeepSkyCamera& camera, GalaxySettings const& settings, std::uint32_t width,
                              std::uint32_t height, double display_exposure, std::string& error) {
    double const tangent = std::tan(camera.vertical_fov_rad * 0.5);
    double const aspect = static_cast<double>(width) / static_cast<double>(std::max(height, 1U));
    pixel_angle_ = 2.0 * tangent / static_cast<double>(std::max(height, 1U));

    // The camera orbits the point it looks at, which moves from the previous
    // focus to the new one while a transition runs.
    math::Vec3d const target = focusPosition(settings.focus).value_or(math::Vec3d{0.0, 0.0, 0.0});
    math::Vec3d centre = target;
    if (settings.transition < 1.0f) {
        math::Vec3d const from = focusPosition(settings.previous_focus).value_or(target);
        double const s = smoothstep(0.0, 1.0, settings.transition);
        centre = from + (target - from) * s;
    }
    view_centre_ = centre;
    camera.position = camera.position + centre;
    camera_world_ = camera.position;
    camera_forward_ = camera.forward;
    camera_right_ = camera.right * (tangent * aspect);
    camera_up_ = camera.up * tangent;

    buildSystemView(settings);
    selectNear(settings, pixel_angle_, display_exposure);
    // The background's stars stand for the foreground of our own galaxy when
    // another is seen from outside; inside this one they are its own stars.
    double const outside = smoothstep(20.0, 35.0, math::length(camera_world_));
    sky_scale_ = static_cast<float>(outside * pre_exposure_);
    status_.pre_exposure = pre_exposure_;
    return uploadView(error);
}

void GalaxyScene::adaptExposure(double target) {
    // Within about a third of a second, on a log scale.
    if (adapt_at_once_ || !(pre_exposure_ > 0.0)) {
        pre_exposure_ = target;
        return;
    }
    double const k = 1.0 - std::exp(-last_elapsed_ / 0.35);
    pre_exposure_ = std::exp(std::log(pre_exposure_) + (std::log(target) - std::log(pre_exposure_)) * k);
}

void GalaxyScene::buildSystemView(GalaxySettings const& settings) {
    draw_system_ = false;
    bodies_.clear();
    if (!settings.focus.hasStar() || !has_front_ || runner_ == nullptr ||
        settings.focus.particle >= front_state_.particles.size()) {
        system_.reset();
        adaptExposure(1.0);
        return;
    }
    StarKey const key{settings.focus.particle, settings.focus.rank};
    math::Vec3d const particle = particlePosition(key.particle);
    ParticleStars const stars = starsOf(key.particle, particle);
    if (key.rank >= stars.count) {
        system_.reset();
        adaptExposure(1.0);
        return;
    }
    // The star evolves with the display time; its system is worked out again
    // when it has aged by a megayear.
    if (!system_ || system_->key != key || std::abs(system_->star.age_myr - stars.age_myr) > 1.0) {
        system_ = generateStarSystem(stars, key.rank);
        system_particle_ = stars;
    }
    StarSystem const& system = *system_;
    StarState const& star = system.star;
    math::Vec3d const star_world = particle + starOffsetPc(stars, key.rank) * 1.0e-3;
    double const coarse = displayCoarseSeconds();
    double const fine = display_fine_s_;
    auto const& axes = system.axes;
    auto toWorld = [&](math::Vec3d const& origin, math::Vec3d const& local_kpc) {
        return origin + axes[0] * local_kpc[0] + axes[1] * local_kpc[1] + axes[2] * local_kpc[2];
    };
    bodies_.push_back({star_world, star.radius * kKmPerSolarRadius / kKmPerKpc,
                       GalaxyFocus{GalaxyFocus::Kind::Star, key.particle, key.rank, 0, 0}});
    for (std::uint32_t p = 0; p < system.planet_count; ++p) {
        Planet const& planet = system.planets[p];
        math::Vec3d const planet_world = toWorld(star_world, planetPosition(planet, coarse, fine) * (kKmPerAu / kKmPerKpc));
        bodies_.push_back({planet_world, planet.radius_earth * kKmPerEarthRadius / kKmPerKpc,
                           GalaxyFocus{GalaxyFocus::Kind::Planet, key.particle, key.rank, p, 0}});
        for (std::uint32_t m = 0; m < planet.moon_count; ++m) {
            Moon const& moon = planet.moons[m];
            math::Vec3d const in_equator = moonPosition(moon, coarse, fine) / kKmPerKpc;
            math::Vec3d const local = planet.equator[0] * in_equator[0] + planet.equator[1] * in_equator[1] +
                                      planet.equator[2] * in_equator[2];
            bodies_.push_back({toWorld(planet_world, local),
                               moon.radius_km / kKmPerKpc,
                               GalaxyFocus{GalaxyFocus::Kind::Moon, key.particle, key.rank, p, m}});
        }
    }

    // Visible light of the star in solar units, and its colour.
    double const log_luminance = star.temperature_k > 0.0
                                     ? blackhole::sampleBlackbodyLogLuminance(blackbody_, star.temperature_k)
                                     : -1.0e30;
    double const light = star.radius * star.radius * std::exp(std::max(log_luminance - solar_log_luminance_, -600.0));
    double const distance_pc = math::length(camera_world_ - star_world) * 1000.0;
    // Exposure: far from the star, the galaxy's; near it, the star's surface
    // when the star is what is looked at, otherwise what the star lights (a
    // surface of albedo 0.3 at the camera's distance from it).
    double target = 1.0;
    if (light > 0.0) {
        double const lit = 0.3 * light / (kPi * std::max(distance_pc * distance_pc, 1.0e-30));
        double const for_lit = std::min(1.0, 0.5 / (lit * kLightScale));
        target = for_lit;
        if (settings.focus.kind == GalaxyFocus::Kind::Star) {
            // A resolved disk is exposed for its surface, a point for the
            // peak of its image.
            double const radius_pc = std::max(star.radius * kPcPerSolarRadius, 1.0e-30);
            double const surface = light / (kPi * radius_pc * radius_pc);
            double const sigma_pc = kStarSigmaPixels * pixel_angle_ * std::max(distance_pc, 1.0e-30);
            double const point = light / (2.0 * kPi * sigma_pc * sigma_pc);
            double const radius_px = radius_pc / std::max(distance_pc, 1.0e-30) / pixel_angle_;
            double const resolved = smoothstep(1.0, 8.0, radius_px);
            double const for_star =
                std::exp(std::log(1.5 / (point * kLightScale)) * (1.0 - resolved) +
                         std::log(0.6 / (surface * kLightScale)) * resolved);
            double const s = smoothstep(std::log(0.01), std::log(1.0), std::log(std::max(distance_pc, 1.0e-30)));
            target = std::exp(std::log(for_star) * (1.0 - s) + std::log(for_lit) * s);
        }
    }
    adaptExposure(target);
    if (distance_pc > kSystemReachPc) {
        return;
    }
    draw_system_ = true;

    // The block for star_system.slang, relative to the camera in km.
    shaders::StarSystemBlock& block = system_block_;
    block = {};
    double const scale = kLightScale * pre_exposure_;
    math::Vec3d const chroma = star.temperature_k > 0.0
                                   ? blackhole::sampleBlackbodyTable(blackbody_, star.temperature_k, log_luminance)
                                   : math::Vec3d{0.0, 0.0, 0.0};
    double const radius_km = std::max(star.radius * kKmPerSolarRadius, 1.0e-3);
    double const radius_pc = radius_km / kKmPerPc;
    double const limb = std::clamp(0.6 + 0.25 * (5772.0 - star.temperature_k) / 2000.0, 0.3, 0.85);
    // Mean radiance over the disk, L / (pi R^2) on the galaxy's scale, and its
    // centre under linear limb darkening.
    double const centre = light / (kPi * radius_pc * radius_pc) / (1.0 - limb / 3.0) * scale;
    math::Vec3d const star_relative = (star_world - camera_world_) * kKmPerKpc;
    store(block.star[0], star_relative, radius_km);
    store(block.star[1], chroma * centre, limb);
    std::size_t const count = std::min(bodies_.size() - 1U, shaders::StarSystemBlock::kMaximumBodies);
    // A unit-albedo surface facing the star at d km: albedo L / (pi d_pc^2).
    store(block.star[2], chroma * (light * scale * kKmPerPc * kKmPerPc / kPi), static_cast<double>(count));
    // Thermal light on the scale of the solar surface's radiance.
    double const solar_surface = 1.0 / (kPi * kPcPerSolarRadius * kPcPerSolarRadius);
    set(block.star[3], 0.0, 0.0, 0.0, solar_surface * scale);
    std::size_t row = 0;
    std::size_t body_index = 1;
    for (std::uint32_t p = 0; p < system.planet_count && row < count; ++p) {
        Planet const& planet = system.planets[p];
        GalaxyBody const& body = bodies_[body_index++];
        store(block.bodies[4 * row], (body.world_kpc - camera_world_) * kKmPerKpc, body.radius_kpc * kKmPerKpc);
        // Day sides of lava worlds and the hottest giants glow: the
        // substellar temperature without redistribution, sqrt(2) T_eq.
        bool const molten = planet.kind == PlanetKind::Lava ||
                            (planet.kind == PlanetKind::GasGiant && planet.equilibrium_k > 1000.0);
        store(block.bodies[4 * row + 1], planet.albedo, molten ? std::sqrt(2.0) * planet.equilibrium_k : 0.0);
        double const planet_km = planet.radius_earth * kKmPerEarthRadius;
        set(block.bodies[4 * row + 2], planet.ring_inner * planet_km, planet.ring_outer * planet_km,
            planet.rings ? 1.0 : 0.0, 0.0);
        math::Vec3d const pole = planet.equator[2];
        store(block.bodies[4 * row + 3], axes[0] * pole[0] + axes[1] * pole[1] + axes[2] * pole[2], 0.0);
        ++row;
        for (std::uint32_t m = 0; m < planet.moon_count && row < count; ++m) {
            GalaxyBody const& moon_body = bodies_[body_index++];
            store(block.bodies[4 * row], (moon_body.world_kpc - camera_world_) * kKmPerKpc,
                  moon_body.radius_kpc * kKmPerKpc);
            store(block.bodies[4 * row + 1], planet.moons[m].albedo, 0.0);
            set(block.bodies[4 * row + 2], 0.0, 0.0, 0.0, 0.0);
            set(block.bodies[4 * row + 3], 0.0, 0.0, 1.0, 0.0);
            ++row;
        }
    }
    block.star[2][3] = static_cast<float>(row);
}

void GalaxyScene::selectNear(GalaxySettings const& settings, double pixel_angle, double display_exposure) {
    near_.clear();
    status_.near_particles = 0;
    status_.resolved_stars = 0;
    if (!settings.resolve_stars || !has_front_ || runner_ == nullptr || front_state_.cell_first.empty()) {
        return;
    }
    // A star of luminosity L at d pc peaks at L / (2 pi (sigma theta d)^2)
    // on the galaxy's scale; it is drawn when that reaches the visible
    // fraction of display white.
    double const sigma = kStarSigmaPixels;
    double const exposure = std::max(kLightScale * pre_exposure_ * display_exposure, 1.0e-30);
    double const flux = kVisibleRadiance * 2.0 * kPi * sigma * sigma * pixel_angle * pixel_angle / exposure *
                        budget_scale_;
    status_.faintest_luminosity = flux;
    // Particles further than a 1000-solar-luminosity giant is seen are left
    // whole.
    double reach = std::min(kMaxReachKpc, std::sqrt(1000.0 / flux) * 1.0e-3);
    math::Vec3d const& eye = camera_world_;
    if (reach <= 1.0e-6 || std::abs(eye[2]) > reach + 3.0) {
        return;
    }

    // Cells within reach in the plane, nearest first, up to a number of
    // particles.
    constexpr std::uint32_t kCells = GalaxySnapshot::kCells;
    double const cell = 2.0 * shaders::kGalaxyPositionKpc / kCells;
    auto cellIndex = [&](double v) {
        return static_cast<int>(std::floor((v + shaders::kGalaxyPositionKpc) / cell));
    };
    int const x0 = std::max(cellIndex(eye[0] - reach), 0);
    int const x1 = std::min(cellIndex(eye[0] + reach), static_cast<int>(kCells) - 1);
    int const y0 = std::max(cellIndex(eye[1] - reach), 0);
    int const y1 = std::min(cellIndex(eye[1] + reach), static_cast<int>(kCells) - 1);
    struct Candidate {
        double distance = 0.0;
        std::uint32_t cell = 0;
    };
    std::vector<Candidate> cells;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            double const left = -shaders::kGalaxyPositionKpc + x * cell;
            double const bottom = -shaders::kGalaxyPositionKpc + y * cell;
            double const dx = std::max({left - eye[0], 0.0, eye[0] - (left + cell)});
            double const dy = std::max({bottom - eye[1], 0.0, eye[1] - (bottom + cell)});
            double const distance = std::hypot(dx, dy);
            if (distance <= reach) {
                cells.push_back({distance, static_cast<std::uint32_t>(y) * kCells + static_cast<std::uint32_t>(x)});
            }
        }
    }
    std::sort(cells.begin(), cells.end(), [](Candidate const& a, Candidate const& b) {
        return a.distance < b.distance || (a.distance == b.distance && a.cell < b.cell);
    });
    std::size_t candidates = 0;
    std::size_t used = 0;
    for (; used < cells.size(); ++used) {
        std::uint32_t const c = cells[used].cell;
        std::size_t const in_cell = front_state_.cell_first[c + 1U] - front_state_.cell_first[c];
        if (candidates + in_cell > kMaxNearCandidates && used > 0) {
            reach = std::min(reach, cells[used].distance);
            break;
        }
        candidates += in_cell;
    }
    near_radius_kpc_ = reach;
    status_.near_radius_kpc = reach;

    // Each particle within reach: how many of its stars are bright enough.
    std::vector<std::uint32_t> list;
    list.reserve(candidates);
    for (std::size_t index = 0; index < used; ++index) {
        std::uint32_t const c = cells[index].cell;
        for (std::uint32_t k = front_state_.cell_first[c]; k < front_state_.cell_first[c + 1U]; ++k) {
            list.push_back(front_state_.cell_particles[k]);
        }
    }
    constexpr std::size_t kChunks = 24;
    std::array<std::vector<NearRecord>, kChunks> found{};
    double const dust = kDustOpacity * std::max(settings.dust, 0.0f);
    view_pool_.run(kChunks, [&](std::size_t chunk) {
        std::size_t const begin = list.size() * chunk / kChunks;
        std::size_t const end = list.size() * (chunk + 1U) / kChunks;
        for (std::size_t k = begin; k < end; ++k) {
            std::uint32_t const i = list[k];
            math::Vec3d const position = particlePosition(i);
            math::Vec3d const offset = eye - position;
            double const distance = math::length(offset);
            if (distance > reach) {
                continue;
            }
            ParticleStars const stars = starsOf(i, position);
            double const distance_pc = distance * 1000.0;
            // The nearest of its stars are closer than its centre.
            double const nearest_pc = std::max({distance_pc - 2.0 * stars.spread_pc, 0.25 * distance_pc, 1.0e-3});
            std::uint32_t const slots =
                std::min(slotsBrighterThan(stars, flux * nearest_pc * nearest_pc), kMaxSlotsPerParticle);
            if (slots == 0) {
                continue;
            }
            NearRecord record;
            record.world_kpc = position;
            record.stars = stars;
            record.slots = slots;
            record.fade = static_cast<float>(1.0 - smoothstep(0.75 * reach, reach, distance));
            math::Vec3d const to_camera = distance > 0.0 ? offset / distance : math::Vec3d{0.0, 0.0, 1.0};
            double const embedded = 1.0 - smoothstep(0.8 * kBirthCloudMyr, 1.2 * kBirthCloudMyr, stars.age_myr);
            record.optical_depth = static_cast<float>(
                dust * gasColumn(position, to_camera) + dust * (kBirthCloudDepth / kDustOpacity) * embedded);
            found[chunk].push_back(record);
        }
    });
    std::uint64_t total = 0;
    for (auto& part : found) {
        for (NearRecord const& record : part) {
            total += record.slots;
        }
    }
    // Over budget: the faintest are left in their particles' light, here by
    // trimming every particle in proportion, and from the next frame by a
    // brighter threshold.
    double const budget = std::max<double>(settings.star_budget, 1.0);
    double const over = static_cast<double>(total) / budget;
    double const trim = over > 1.0 ? 1.0 / over : 1.0;
    if (over > 1.0) {
        budget_scale_ = std::min(budget_scale_ * std::pow(over, 1.2), 1.0e12);
    } else if (over < 0.6) {
        budget_scale_ = std::max(budget_scale_ * 0.92, 1.0);
    }
    std::uint64_t drawn = 0;
    for (auto& part : found) {
        for (NearRecord& record : part) {
            record.slots = static_cast<std::uint32_t>(std::floor(record.slots * trim));
            if (record.slots > 0) {
                drawn += record.slots;
                near_.push_back(record);
            }
        }
    }
    status_.near_particles = static_cast<std::uint32_t>(near_.size());
    status_.resolved_stars = drawn;
}

bool GalaxyScene::uploadView(std::string& error) {
    // Records and chunks for galaxy_resolved_stars.slang.
    near_upload_.clear();
    chunk_upload_.clear();
    // The visited star is drawn by the system pass when that runs, else as
    // one of the single stars.
    for (std::size_t index = 0; index < near_.size(); ++index) {
        NearRecord const& record = near_[index];
        shaders::GalaxyNearParticle gpu{};
        math::Vec3d const relative = (record.world_kpc - camera_world_) * 1000.0;
        set(gpu.position_age, relative[0], relative[1], relative[2], record.stars.age_myr);
        set(gpu.shape, record.stars.spread_pc, record.stars.thickness_pc, record.fade, record.optical_depth);
        gpu.stars[0] = record.stars.particle;
        gpu.stars[1] = record.stars.count;
        gpu.stars[2] = record.stars.dead;
        gpu.stars[3] = record.slots;
        near_upload_.push_back(gpu);
        for (std::uint32_t first = 0; first < record.slots; first += shaders::kGalaxyStarChunk) {
            chunk_upload_.push_back(static_cast<std::uint32_t>(index));
            chunk_upload_.push_back(first);
        }
    }
    star_instances_ = static_cast<std::uint32_t>(chunk_upload_.size() / 2U) * shaders::kGalaxyStarChunk;

    bool rebind = star_resources_ == nullptr;
    if (near_upload_.size() > near_capacity_ || near_buffer_ == nullptr) {
        near_capacity_ = std::max<std::size_t>(near_upload_.size() * 3U / 2U, 1024U);
        near_buffer_ = createStructuredBuffer(device_, "Helios galaxy near particles", near_capacity_,
                                              sizeof(shaders::GalaxyNearParticle));
        rebind = true;
    }
    std::size_t const chunks = chunk_upload_.size() / 2U;
    if (chunks > chunk_capacity_ || chunk_buffer_ == nullptr) {
        chunk_capacity_ = std::max<std::size_t>(chunks * 3U / 2U, 4096U);
        chunk_buffer_ = createStructuredBuffer(device_, "Helios galaxy star chunks", chunk_capacity_, 2U * sizeof(std::uint32_t));
        rebind = true;
    }
    if (near_buffer_ == nullptr || chunk_buffer_ == nullptr) {
        error = "Diligent failed to create the single-star buffers";
        return false;
    }
    if (rebind) {
        star_resources_.Release();
        star_pipeline_->CreateShaderResourceBinding(&star_resources_, true);
        // The buffers are read by the vertex stage.
        auto* const records = star_resources_ != nullptr
                                  ? star_resources_->GetVariableByName(Diligent::SHADER_TYPE_VERTEX, "near_particles")
                                  : nullptr;
        auto* const chunk_variable = star_resources_ != nullptr
                                         ? star_resources_->GetVariableByName(Diligent::SHADER_TYPE_VERTEX, "star_chunks")
                                         : nullptr;
        if (records == nullptr || chunk_variable == nullptr) {
            error = "Diligent could not bind the single-star buffers";
            return false;
        }
        records->Set(near_buffer_->GetDefaultView(Diligent::BUFFER_VIEW_SHADER_RESOURCE));
        chunk_variable->Set(chunk_buffer_->GetDefaultView(Diligent::BUFFER_VIEW_SHADER_RESOURCE));
    }
    if (!near_upload_.empty()) {
        context_->UpdateBuffer(near_buffer_, 0, near_upload_.size() * sizeof(shaders::GalaxyNearParticle),
                               near_upload_.data(), Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        context_->UpdateBuffer(chunk_buffer_, 0, chunk_upload_.size() * sizeof(std::uint32_t), chunk_upload_.data(),
                               Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }

    // What each particle's image keeps: the light of the stars not drawn.
    if (unresolved_buffer_ != nullptr && unresolved_.size() == front_state_.particles.size()) {
        for (std::uint32_t index : unresolved_set_) {
            unresolved_[index] = 255U;
        }
        bool const had = !unresolved_set_.empty();
        unresolved_set_.clear();
        for (NearRecord const& record : near_) {
            double const resolved = resolved_light_.fraction(
                                        record.stars.age_myr,
                                        static_cast<double>(record.slots) / static_cast<double>(record.stars.count)) *
                                    record.fade;
            auto const value = static_cast<std::uint8_t>(std::lround(std::clamp(1.0 - resolved, 0.0, 1.0) * 255.0));
            unresolved_[record.stars.particle] = value;
            unresolved_set_.push_back(record.stars.particle);
        }
        if (had || !unresolved_set_.empty() || unresolved_dirty_) {
            context_->UpdateBuffer(unresolved_buffer_, 0, unresolved_.size(), unresolved_.data(),
                                   Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            unresolved_dirty_ = false;
        }
    }

    if (draw_system_) {
        Diligent::MapHelper<shaders::StarSystemBlock> mapped{context_, system_buffer_, Diligent::MAP_WRITE,
                                                            Diligent::MAP_FLAG_DISCARD};
        *mapped = system_block_;
    }
    return true;
}

void GalaxyScene::fillFrame(shaders::DeepSkyFrame& frame, GalaxySettings const& settings) const {
    if (runner_ == nullptr) {
        return;
    }
    GalaxyRecipe const& recipe = runner_->recipe();
    double const display = displayMyr();
    set(frame.object[0], display, disk_stars_, bulge_stars_, 0.0);
    set(frame.object[1], runner_->diskParticleMass(), runner_->bulgeParticleMass(),
        runner_->cloudParticleMass(), kLightScale * pre_exposure_);
    set(frame.object[2], 0.5 / recipe.half_extent_kpc, recipe.bulge_scale_kpc,
        recipe.bulge_mass * 1.0e10, kDustOpacity * std::max(settings.dust, 0.0f));
    double const log_minimum = std::log(population_.min_age_myr);
    set(frame.object[3], log_minimum, 1.0 / (std::log(population_.max_age_myr) - log_minimum),
        population_.count, kSmallestSigmaPixels);
    double const gas = std::max(settings.ionised_gas, 0.0f);
    set(frame.object[4], ionised_light_[0] * gas, ionised_light_[1] * gas, ionised_light_[2] * gas,
        std::clamp(settings.smoothing, 0.25f, 8.0f));
    // How far each particle has moved since the drawn state was published.
    set(frame.object[5], shaders::kGalaxyPositionKpc,
        static_cast<double>(shaders::kGalaxyVelocityKmS) * kKpcPerMyrPerKmS,
        std::clamp(display - front_time_myr_, -5.0, 5.0), 0.0);
    // The visited star, drawn by the system pass while it runs.
    bool const excluded = draw_system_ && system_.has_value();
    set(frame.object[6], excluded ? static_cast<double>(system_->key.particle) : -1.0,
        excluded ? static_cast<double>(system_->key.rank) : -1.0, solar_log_luminance_, kStarSigmaPixels);
    frame.sky[0] *= sky_scale_;
    frame.sky[1] *= sky_scale_;
}

void GalaxyScene::draw(shaders::DeepSkyFrame const& frame, Pass pass) {
    if (!has_front_ || resources_ == nullptr || front_particles_ == 0 || unresolved_buffer_ == nullptr) {
        return;
    }
    Diligent::IBuffer* buffers[] = {particles_[front_], unresolved_buffer_};
    context_->SetVertexBuffers(0, 2, buffers, nullptr,
                               Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                               Diligent::SET_VERTEX_BUFFERS_FLAG_RESET);
    context_->SetPipelineState(pipeline_);
    {
        Diligent::MapHelper<shaders::DeepSkyFrame> mapped{context_, frame_buffer_,
                                                         Diligent::MAP_WRITE,
                                                         Diligent::MAP_FLAG_DISCARD};
        *mapped = frame;
        mapped->object[0][3] = static_cast<float>(pass);
    }
    context_->CommitShaderResources(resources_, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    // Disk and bulge stars were born before the run began, so once it has
    // lasted longer than a population stays young or ionising, only stars
    // formed during the run can appear in those two passes.
    std::uint32_t first = 0;
    if (pass != Pass::OldStars && displayMyr() > kYoungestInitialMyr) {
        first = std::min(disk_stars_ + bulge_stars_, front_particles_);
    }
    Diligent::DrawAttribs attribs;
    attribs.NumVertices = 4;
    attribs.NumInstances = front_particles_ - first;
    attribs.FirstInstanceLocation = first;
    attribs.Flags = Diligent::DRAW_FLAG_VERIFY_ALL;
    if (attribs.NumInstances > 0) {
        context_->Draw(attribs);
    }
}

void GalaxyScene::drawResolvedStars() {
    if (star_instances_ == 0 || star_resources_ == nullptr) {
        return;
    }
    context_->SetPipelineState(star_pipeline_);
    context_->CommitShaderResources(star_resources_, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Diligent::DrawAttribs attribs;
    attribs.NumVertices = 4;
    attribs.NumInstances = star_instances_;
    attribs.Flags = Diligent::DRAW_FLAG_VERIFY_ALL;
    context_->Draw(attribs);
}

void GalaxyScene::drawSystem(Diligent::ITextureView* target, std::uint32_t width, std::uint32_t height) {
    if (!draw_system_ || system_resources_ == nullptr) {
        return;
    }
    drawFullscreen(context_, {&target, 1}, width, height, system_pipeline_, system_resources_);
}

// ---- picking -------------------------------------------------------------------------------

std::optional<GalaxyFocus> GalaxyScene::pick(double x, double y) const {
    if (!has_front_ || runner_ == nullptr) {
        return std::nullopt;
    }
    double const ndc_x = 2.0 * x - 1.0;
    double const ndc_y = 1.0 - 2.0 * y;
    math::Vec3d const ray = math::normalized(camera_forward_ + camera_right_ * ndc_x + camera_up_ * ndc_y);
    auto angleTo = [&](math::Vec3d const& world) {
        math::Vec3d const relative = world - camera_world_;
        double const distance = math::length(relative);
        if (distance <= 0.0 || math::dot(relative, ray) <= 0.0) {
            return 1.0e30;
        }
        return std::atan2(math::length(math::cross(relative, ray)), math::dot(relative, ray));
    };

    // A body of the visited system: within its disk, or eight pixels of it.
    std::optional<GalaxyFocus> best_body;
    double best_score = 1.0e30;
    for (GalaxyBody const& body : bodies_) {
        double const angle = angleTo(body.world_kpc);
        double const distance = math::length(body.world_kpc - camera_world_);
        double const radius = std::asin(std::min(body.radius_kpc / std::max(distance, 1.0e-30), 1.0));
        double const reach = std::max(radius, 8.0 * pixel_angle_);
        if (angle <= reach && angle / reach < best_score) {
            best_score = angle / reach;
            best_body = body.focus;
        }
    }
    if (best_body) {
        return best_body;
    }

    // A single star within six pixels: the brightest one seen.
    double const tolerance = 6.0 * pixel_angle_;
    std::optional<GalaxyFocus> best_star;
    double best_flux = 0.0;
    for (NearRecord const& record : near_) {
        double const spread_kpc = 3.0 * std::max(record.stars.spread_pc, record.stars.thickness_pc) * 1.0e-3;
        double const centre_distance = math::length(record.world_kpc - camera_world_);
        if (centre_distance > spread_kpc &&
            angleTo(record.world_kpc) > std::asin(std::min(spread_kpc / centre_distance, 1.0)) + tolerance) {
            continue;
        }
        for (std::uint32_t slot = 0; slot < record.slots; ++slot) {
            std::uint32_t const rank = rankOfSlot(record.stars, slot);
            math::Vec3d const world = record.world_kpc + starOffsetPc(record.stars, rank) * 1.0e-3;
            if (angleTo(world) > tolerance) {
                continue;
            }
            StarState const star = evolveStar(initialMassOfRank(record.stars, rank), record.stars.age_myr);
            double const distance = math::length(world - camera_world_);
            double const seen = star.luminosity / std::max(distance * distance, 1.0e-30);
            if (seen > best_flux) {
                best_flux = seen;
                best_star = GalaxyFocus{GalaxyFocus::Kind::Star, record.stars.particle, rank, 0, 0};
            }
        }
    }
    if (best_star) {
        return best_star;
    }

    // Otherwise the star particle nearest the click on the screen.
    double best_angle = 12.0 * pixel_angle_;
    std::optional<GalaxyFocus> best_particle;
    std::size_t const count = front_state_.particles.size();
    for (std::size_t i = 0; i < count; ++i) {
        if (front_state_.particles[i].birth == shaders::kGalaxyBirthGas) {
            continue;
        }
        double const angle = angleTo(particlePosition(static_cast<std::uint32_t>(i)));
        if (angle < best_angle) {
            best_angle = angle;
            best_particle = GalaxyFocus{GalaxyFocus::Kind::Particle, static_cast<std::uint32_t>(i), 0, 0, 0};
        }
    }
    return best_particle;
}

std::optional<GalaxyFocus> GalaxyScene::findStar(double radius_kpc, StarSearch search) const {
    if (!has_front_ || runner_ == nullptr) {
        return std::nullopt;
    }
    auto matches = [&](StarSystem const& system) {
        auto any = [&](auto&& test) {
            for (std::uint32_t p = 0; p < system.planet_count; ++p) {
                if (test(system.planets[p])) {
                    return true;
                }
            }
            return false;
        };
        switch (search) {
        case StarSearch::SunLike:
            return system.star.phase == StarPhase::MainSequence && system.planet_count >= 3;
        case StarSearch::RingedGiant:
            return any([](Planet const& planet) { return planet.rings; });
        case StarSearch::LavaWorld:
            return any([](Planet const& planet) { return planet.kind == PlanetKind::Lava; });
        case StarSearch::Giant:
            return system.star.phase == StarPhase::Giant && system.planet_count > 0;
        case StarSearch::WhiteDwarf:
            return system.star.phase == StarPhase::WhiteDwarf && system.planet_count > 0;
        }
        return false;
    };
    // Disk particles at that radius, in index order (deterministic); in each,
    // the ranks where such a star can be.
    std::size_t const count = std::min<std::size_t>(disk_stars_, front_state_.particles.size());
    std::uint32_t searched = 0;
    for (std::size_t i = 0; i < count && searched < 64U; ++i) {
        math::Vec3d const position = particlePosition(static_cast<std::uint32_t>(i));
        if (std::abs(std::hypot(position[0], position[1]) - radius_kpc) > 0.3 || std::abs(position[2]) > 0.3) {
            continue;
        }
        ++searched;
        ParticleStars const stars = starsOf(static_cast<std::uint32_t>(i), position);
        std::uint32_t first = 0;
        std::uint32_t last = 0;
        switch (search) {
        case StarSearch::SunLike:
            first = static_cast<std::uint32_t>(imfTailFraction(1.0) * stars.count);
            last = first + 64U;
            break;
        case StarSearch::RingedGiant:
        case StarSearch::LavaWorld:
            first = static_cast<std::uint32_t>(imfTailFraction(1.4) * stars.count);
            last = std::min(static_cast<std::uint32_t>(imfTailFraction(0.6) * stars.count), first + 4000U);
            break;
        case StarSearch::Giant:
            first = stars.dead;
            last = stars.dead + stars.giants;
            break;
        case StarSearch::WhiteDwarf:
            first = stars.dead > 2000U ? stars.dead - 2000U : 0U;
            last = stars.dead;
            break;
        }
        first = std::max(first, search == StarSearch::WhiteDwarf ? 0U : stars.dead);
        for (std::uint32_t rank = first; rank < std::min(last, stars.count); ++rank) {
            if (search != StarSearch::Giant && search != StarSearch::WhiteDwarf && rank < stars.dead + stars.giants) {
                continue;
            }
            if (matches(generateStarSystem(stars, rank))) {
                return GalaxyFocus{GalaxyFocus::Kind::Star, static_cast<std::uint32_t>(i), rank, 0, 0};
            }
        }
    }
    return std::nullopt;
}

} // namespace helios::examples

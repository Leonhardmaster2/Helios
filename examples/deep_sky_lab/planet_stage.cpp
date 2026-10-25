// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "planet_stage.hpp"

#include "fullscreen_pass.hpp"

#include "shaders/abi.hpp"

#include "MapHelper.hpp"
#include "SwapChain.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <numbers>
#include <utility>

namespace helios::examples {
namespace {

constexpr Diligent::TEXTURE_FORMAT kDepthFormat = Diligent::TEX_FORMAT_D32_FLOAT;
constexpr Diligent::TEXTURE_FORMAT kColorFormat = Diligent::TEX_FORMAT_RGBA16_FLOAT;
// Selections a still camera may need before splits and merges stop.
constexpr int kSettlePasses = 64;
// The transmittance table: heights and sun elevations.
constexpr std::uint32_t kLutHeights = 128;
constexpr std::uint32_t kLutAngles = 256;
constexpr int kLutSteps = 96;
// The multiple-scattering table: heights, sun elevations, directions over
// the sphere and steps along each (Hillaire 2020, section 5.2).
constexpr std::uint32_t kMsHeights = 32;
constexpr std::uint32_t kMsAngles = 64;
constexpr int kMsDirections = 64;
constexpr int kMsSteps = 24;
// Mean albedo of the ground under the atmosphere, for the light it returns
// to the sky (Earth's land and sea average about 0.3 with clouds, 0.15
// without; the clouds are drawn separately).
constexpr double kGroundAlbedo = 0.15;
constexpr std::size_t kRingSamples = 2048;
constexpr double kRadians = std::numbers::pi / 180.0;
// The sun's angular radius as seen from Earth.
constexpr double kSunAngularRadius = 0.004652;
// The lab's star field is in its own units; relative to the sun's
// irradiance, stars are faint.  This keeps them visible at night at a few
// stops over the daytime exposure.
constexpr float kStarScale = 0.02f;

// Element `index` of the van der Corput sequence in `base`, in (0, 1).
[[nodiscard]] double haltonSequence(std::uint32_t index, std::uint32_t base) {
    double f = 1.0;
    double r = 0.0;
    while (index > 0) {
        f /= base;
        r += f * (index % base);
        index /= base;
    }
    return r;
}

// Sub-pixel offset of an accumulated frame; shaders/planet_sky.slang
// computes the same.
[[nodiscard]] std::array<double, 2> frameJitter(std::uint32_t frame_index) {
    std::uint32_t const index = frame_index % 64U + 1U;
    return {haltonSequence(index, 2) - 0.5, haltonSequence(index, 3) - 0.5};
}

void set(float destination[4], double x, double y, double z, double w) {
    destination[0] = static_cast<float>(x);
    destination[1] = static_cast<float>(y);
    destination[2] = static_cast<float>(z);
    destination[3] = static_cast<float>(w);
}

// Optical depth (metres of surface-density path) of molecules and aerosols
// from height h along a ray at sun-elevation cosine mu to the top of the
// atmosphere, or (1e9, 1e9) when the ray meets the ground.
[[nodiscard]] std::array<double, 2> opticalDepthToSpace(PlanetBody const& body, double h, double mu) {
    double const radius = body.radius_m;
    double const top = radius + body.atmosphere_top_m;
    double const r0 = radius + h;
    double const discriminant_top = r0 * r0 * (mu * mu - 1.0) + top * top;
    if (discriminant_top <= 0.0) {
        return {1.0e9, 1.0e9};
    }
    double const t_top = -r0 * mu + std::sqrt(discriminant_top);
    if (mu < 0.0) {
        double const discriminant_ground = r0 * r0 * (mu * mu - 1.0) + radius * radius;
        if (discriminant_ground > 0.0) {
            double const t_ground = -r0 * mu - std::sqrt(discriminant_ground);
            if (t_ground > 0.0) {
                return {1.0e9, 1.0e9};
            }
        }
    }
    double const step = t_top / kLutSteps;
    std::array<double, 2> depth{0.0, 0.0};
    for (int i = 0; i < kLutSteps; ++i) {
        double const t = (static_cast<double>(i) + 0.5) * step;
        double const r = std::sqrt(r0 * r0 + t * t + 2.0 * r0 * t * mu);
        double const height = std::max(r - radius, 0.0);
        depth[0] += std::exp(-height / body.rayleigh_height_m) * step;
        depth[1] += std::exp(-height / body.mie_height_m) * step;
    }
    return depth;
}

// The atmosphere fields the tables depend on.
[[nodiscard]] bool sameAtmosphere(PlanetBody const& a, PlanetBody const& b) {
    return a.radius_m == b.radius_m && a.atmosphere_top_m == b.atmosphere_top_m &&
           a.rayleigh_height_m == b.rayleigh_height_m && a.mie_height_m == b.mie_height_m &&
           a.rayleigh_beta[0] == b.rayleigh_beta[0] && a.rayleigh_beta[1] == b.rayleigh_beta[1] &&
           a.rayleigh_beta[2] == b.rayleigh_beta[2] && a.mie_beta[0] == b.mie_beta[0] &&
           a.mie_beta[1] == b.mie_beta[1] && a.mie_beta[2] == b.mie_beta[2];
}

// Bilinear lookup of the transmittance texels (molecules, aerosols) at a
// height and a sun-elevation cosine, as the shader samples them.
[[nodiscard]] std::array<double, 2> lookupOpticalDepth(std::vector<float> const& texels, double top,
                                                       double height, double mu) {
    double const x = std::clamp(height / top, 0.0, 1.0) * kLutHeights - 0.5;
    double const y = std::clamp(mu * 0.5 + 0.5, 0.0, 1.0) * kLutAngles - 0.5;
    auto const x0 = static_cast<std::uint32_t>(std::clamp(std::floor(x), 0.0, kLutHeights - 1.0));
    auto const y0 = static_cast<std::uint32_t>(std::clamp(std::floor(y), 0.0, kLutAngles - 1.0));
    std::uint32_t const x1 = std::min(x0 + 1, kLutHeights - 1);
    std::uint32_t const y1 = std::min(y0 + 1, kLutAngles - 1);
    double const fx = std::clamp(x - x0, 0.0, 1.0);
    double const fy = std::clamp(y - y0, 0.0, 1.0);
    auto at = [&](std::uint32_t column, std::uint32_t row, std::size_t channel) {
        return static_cast<double>(texels[(static_cast<std::size_t>(row) * kLutHeights + column) * 2U + channel]);
    };
    std::array<double, 2> result{};
    for (std::size_t channel = 0; channel < 2; ++channel) {
        double const top_row = at(x0, y0, channel) * (1.0 - fx) + at(x1, y0, channel) * fx;
        double const bottom_row = at(x0, y1, channel) * (1.0 - fx) + at(x1, y1, channel) * fx;
        result[channel] = top_row * (1.0 - fy) + bottom_row * fy;
    }
    return result;
}

// The multiply scattered light per unit scattering coefficient at a height
// and sun elevation (Hillaire 2020, equations 7 to 10): the light scattered
// once, gathered over all directions with an isotropic phase, plus what the
// ground reflects, divided by one minus the fraction that scatters again.
[[nodiscard]] std::array<double, 3> multipleScatteringAt(PlanetBody const& body,
                                                         std::vector<float> const& transmittance,
                                                         double height, double mu_sun) {
    double const radius = body.radius_m;
    double const top = radius + body.atmosphere_top_m;
    math::Vec3d const origin{0.0, 0.0, radius + height};
    math::Vec3d const to_sun{std::sqrt(std::max(1.0 - mu_sun * mu_sun, 0.0)), 0.0, mu_sun};
    std::array<double, 3> const beta_r{body.rayleigh_beta[0], body.rayleigh_beta[1], body.rayleigh_beta[2]};
    std::array<double, 3> const beta_m{body.mie_beta[0], body.mie_beta[1], body.mie_beta[2]};
    std::array<double, 3> gathered{};
    std::array<double, 3> transfer{};
    constexpr double kGolden = 2.399963229728653; // the golden angle
    for (int d = 0; d < kMsDirections; ++d) {
        // Fibonacci sphere.
        double const z = 1.0 - 2.0 * (d + 0.5) / kMsDirections;
        double const ring = std::sqrt(std::max(1.0 - z * z, 0.0));
        double const phi = kGolden * d;
        math::Vec3d const direction{ring * std::cos(phi), ring * std::sin(phi), z};
        double const b = math::dot(origin, direction);
        double const c_top = math::lengthSq(origin) - top * top;
        double const discriminant_top = b * b - c_top;
        if (discriminant_top <= 0.0) {
            continue;
        }
        double t_max = -b + std::sqrt(discriminant_top);
        bool ground = false;
        double const c_ground = math::lengthSq(origin) - radius * radius;
        double const discriminant_ground = b * b - c_ground;
        if (discriminant_ground > 0.0) {
            double const t_ground = -b - std::sqrt(discriminant_ground);
            if (t_ground > 0.0) {
                t_max = t_ground;
                ground = true;
            }
        }
        if (t_max <= 0.0) {
            continue;
        }
        double const dt = t_max / kMsSteps;
        std::array<double, 3> view{1.0, 1.0, 1.0};
        for (int i = 0; i < kMsSteps; ++i) {
            double const t = (i + 0.5) * dt;
            math::Vec3d const x = origin + direction * t;
            double const r = math::length(x);
            double const h = std::max(r - radius, 0.0);
            double const density_r = std::exp(-h / body.rayleigh_height_m);
            double const density_m = std::exp(-h / body.mie_height_m);
            double const mu = math::dot(x, to_sun) / std::max(r, 1.0);
            auto const depth_sun = lookupOpticalDepth(transmittance, body.atmosphere_top_m, h, mu);
            for (std::size_t k = 0; k < 3; ++k) {
                double const scattering = beta_r[k] * density_r + beta_m[k] * density_m;
                double const sun = depth_sun[0] > 1.0e8 ? 0.0
                                                        : std::exp(-(beta_r[k] * depth_sun[0] + beta_m[k] * depth_sun[1]));
                double const half = std::exp(-0.5 * scattering * dt);
                double const mid = view[k] * half;
                gathered[k] += mid * scattering * sun * dt / (4.0 * std::numbers::pi);
                transfer[k] += mid * scattering * dt;
                view[k] *= half * half;
            }
        }
        if (ground) {
            math::Vec3d const x = origin + direction * t_max;
            double const r = math::length(x);
            double const mu = math::dot(x, to_sun) / std::max(r, 1.0);
            auto const depth_sun = lookupOpticalDepth(transmittance, body.atmosphere_top_m, 0.0, mu);
            for (std::size_t k = 0; k < 3; ++k) {
                double const sun = depth_sun[0] > 1.0e8 ? 0.0
                                                        : std::exp(-(beta_r[k] * depth_sun[0] + beta_m[k] * depth_sun[1]));
                gathered[k] += view[k] * (kGroundAlbedo / std::numbers::pi) * std::max(mu, 0.0) * sun;
            }
        }
    }
    std::array<double, 3> result{};
    for (std::size_t k = 0; k < 3; ++k) {
        double const second_order = gathered[k] / kMsDirections;
        double const fraction = std::min(transfer[k] / kMsDirections, 0.95);
        result[k] = second_order / (1.0 - fraction);
    }
    return result;
}

// Rotation about z by `angle`, applied to a vector.
[[nodiscard]] math::Vec3d rotateZ(math::Vec3d const& v, double angle) {
    double const c = std::cos(angle);
    double const s = std::sin(angle);
    return {v[0] * c - v[1] * s, v[0] * s + v[1] * c, v[2]};
}

// Colour of the sky's ambient light: the Rayleigh coefficient, normalised
// to unit mean (bluish), or white without an atmosphere.
[[nodiscard]] math::Vec3d ambientColor(PlanetBody const& body) {
    if (!body.atmosphere) {
        return {1.0, 1.0, 1.0};
    }
    math::Vec3d const beta = body.rayleigh_beta + body.mie_beta;
    double const mean = std::max((beta[0] + beta[1] + beta[2]) / 3.0, 1.0e-12);
    // Half the light in a shadow is the sky's, half comes off the lit
    // ground around it.
    return (beta / mean + math::Vec3d{1.0, 1.0, 1.0}) * 0.5;
}

} // namespace

bool PlanetStage::initialize(Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                             Diligent::TEXTURE_FORMAT output_format,
                             Diligent::ITextureView* blackbody_table,
                             blackhole::BlackbodyTable const& table, std::string& error) {
    device_ = device;
    context_ = context;
    blackbody_view_ = blackbody_table;
    blackbody_ = table;
    Diligent::SwapChainDesc formats;
    formats.ColorBufferFormat = kColorFormat;
    formats.DepthBufferFormat = kDepthFormat;
    if (!renderer_.initialize(device_, context_, formats, error)) {
        return false;
    }
    return createSkyPass(output_format, error);
}

bool PlanetStage::ensureSelector(Body& body, double radius_m, float lod_error_px, std::string& error) {
    if (body.selector.has_value() && body.radius_m == radius_m && body.lod_error_px == lod_error_px) {
        return true;
    }
    planet::QuadtreeLodConfig config = defaultPlanetLod(radius_m);
    config.split_threshold_px = std::clamp(static_cast<double>(lod_error_px), 0.5, 16.0);
    config.merge_threshold_px = config.split_threshold_px * (2.0 / 3.0);
    // A smaller body needs fewer levels for the same ground detail.
    config.root_geometric_error_m = std::max(radius_m * 0.03, 10.0);
    if (body.selector.has_value() && body.radius_m == radius_m) {
        auto changed = body.selector->setConfig(config, true);
        if (changed.failed()) {
            error = "planet LOD policy: " + changed.error().to_string();
            return false;
        }
    } else {
        auto selector = planet::QuadtreeLodSelector::create(config);
        if (selector.failed()) {
            error = "planet LOD selector: " + selector.error().to_string();
            return false;
        }
        body.selector.emplace(std::move(selector.value()));
    }
    body.radius_m = radius_m;
    body.lod_error_px = lod_error_px;
    return true;
}

bool PlanetStage::createSkyPass(Diligent::TEXTURE_FORMAT output_format, std::string& error) {
    frame_buffer_ = createDynamicUniformBuffer(device_, "Helios planet sky frame constants",
                                               sizeof(shaders::DeepSkyFrame));
    body_buffer_ = createDynamicUniformBuffer(device_, "Helios planet sky body constants",
                                              sizeof(shaders::PlanetBodyParams));
    extra_buffer_ = createDynamicUniformBuffer(device_, "Helios planet sky extra constants",
                                               sizeof(shaders::PlanetSkyExtra));
    Diligent::SamplerDesc sampler;
    sampler.Name = "Helios planet sky linear clamp sampler";
    sampler.MinFilter = sampler.MagFilter = Diligent::FILTER_TYPE_LINEAR;
    sampler.MipFilter = Diligent::FILTER_TYPE_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = Diligent::TEXTURE_ADDRESS_CLAMP;
    device_->CreateSampler(sampler, &linear_sampler_);
    if (frame_buffer_ == nullptr || body_buffer_ == nullptr || extra_buffer_ == nullptr ||
        linear_sampler_ == nullptr) {
        error = "Diligent failed to create the planet sky constant buffers or sampler";
        return false;
    }
    fullscreen_vertex_ = createEmbeddedShader(device_, "fullscreen.slang.vertex.spv",
                                              "Helios planet sky vertex shader",
                                              Diligent::SHADER_TYPE_VERTEX, error);
    auto pixel = createEmbeddedShader(device_, "planet_sky.slang.fragment.spv",
                                      "Helios planet sky pixel shader", Diligent::SHADER_TYPE_PIXEL,
                                      error);
    if (fullscreen_vertex_ == nullptr || pixel == nullptr) {
        return false;
    }
    std::array const formats{output_format};
    Diligent::ShaderResourceVariableDesc const variables[] = {
        {Diligent::SHADER_TYPE_PIXEL, "terrain_depth", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {Diligent::SHADER_TYPE_PIXEL, "terrain_color", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {Diligent::SHADER_TYPE_PIXEL, "transmittance_lut", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {Diligent::SHADER_TYPE_PIXEL, "ring_profile", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {Diligent::SHADER_TYPE_PIXEL, "multiscatter_lut", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
    };
    FullscreenPipelineInfo info;
    info.name = "Helios planet sky pipeline";
    info.vertex_shader = fullscreen_vertex_;
    info.pixel_shader = pixel;
    info.target_formats = formats;
    info.variables = variables;
    sky_pipeline_ = createFullscreenPipeline(device_, info);
    if (sky_pipeline_ == nullptr) {
        error = "Diligent failed to create the planet sky pipeline";
        return false;
    }
    if (!bindStaticVariable(sky_pipeline_, "frame", frame_buffer_, error) ||
        !bindStaticVariable(sky_pipeline_, "blackbody_table", blackbody_view_, error)) {
        return false;
    }
    // Blocks the compiler keeps only when something in the pass reads them.
    if (auto* body = sky_pipeline_->GetStaticVariableByName(Diligent::SHADER_TYPE_PIXEL, "body")) {
        body->Set(body_buffer_);
    }
    if (auto* extra = sky_pipeline_->GetStaticVariableByName(Diligent::SHADER_TYPE_PIXEL, "extra")) {
        extra->Set(extra_buffer_);
    }
    sky_pipeline_->CreateShaderResourceBinding(&sky_resources_, true);
    if (sky_resources_ == nullptr) {
        error = "Diligent failed to create the planet sky resource binding";
        return false;
    }
    return true;
}

bool PlanetStage::ensureTargets(std::uint32_t width, std::uint32_t height, std::string& error) {
    if (depth_ != nullptr && width == target_width_ && height == target_height_) {
        return true;
    }
    depth_.Release();
    color_.Release();
    Diligent::TextureDesc desc;
    desc.Name = "Helios lab planet depth";
    desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
    desc.Width = width;
    desc.Height = height;
    desc.Format = kDepthFormat;
    desc.BindFlags = Diligent::BIND_DEPTH_STENCIL | Diligent::BIND_SHADER_RESOURCE;
    device_->CreateTexture(desc, nullptr, &depth_);
    color_ = createRenderTexture(device_, "Helios lab planet terrain radiance", width, height,
                                 kColorFormat, nullptr);
    if (depth_ == nullptr || color_ == nullptr) {
        error = "Diligent failed to create the planet terrain targets";
        return false;
    }
    target_width_ = width;
    target_height_ = height;
    return true;
}

bool PlanetStage::ensureAtmosphereTables(PlanetBody const& body, std::string& error) {
    if (transmittance_ != nullptr && multiscatter_ != nullptr && transmittance_body_.has_value() &&
        sameAtmosphere(*transmittance_body_, body)) {
        return true;
    }
    std::vector<float> texels(static_cast<std::size_t>(kLutHeights) * kLutAngles * 2U);
    for (std::uint32_t row = 0; row < kLutAngles; ++row) {
        double const mu = (static_cast<double>(row) + 0.5) / kLutAngles * 2.0 - 1.0;
        for (std::uint32_t column = 0; column < kLutHeights; ++column) {
            double const height = (static_cast<double>(column) + 0.5) / kLutHeights * body.atmosphere_top_m;
            auto const depth = opticalDepthToSpace(body, height, mu);
            std::size_t const index = (static_cast<std::size_t>(row) * kLutHeights + column) * 2U;
            texels[index] = static_cast<float>(depth[0]);
            texels[index + 1] = static_cast<float>(depth[1]);
        }
    }
    Diligent::TextureDesc desc;
    desc.Name = "Helios planet transmittance table";
    desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
    desc.Width = kLutHeights;
    desc.Height = kLutAngles;
    desc.MipLevels = 1;
    desc.Format = Diligent::TEX_FORMAT_RG32_FLOAT;
    desc.Usage = Diligent::USAGE_IMMUTABLE;
    desc.BindFlags = Diligent::BIND_SHADER_RESOURCE;
    Diligent::TextureSubResData level;
    level.pData = texels.data();
    level.Stride = static_cast<Diligent::Uint64>(kLutHeights) * 2U * sizeof(float);
    Diligent::TextureData data;
    data.pSubResources = &level;
    data.NumSubresources = 1;
    transmittance_.Release();
    device_->CreateTexture(desc, &data, &transmittance_);
    if (transmittance_ == nullptr) {
        error = "Diligent failed to create the planet transmittance table";
        return false;
    }
    transmittance_->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE)->SetSampler(linear_sampler_);

    // The multiple scattering, from the transmittance just computed.
    std::vector<float> ms_texels(static_cast<std::size_t>(kMsHeights) * kMsAngles * 4U, 0.0f);
    for (std::uint32_t row = 0; row < kMsAngles; ++row) {
        double const mu = (static_cast<double>(row) + 0.5) / kMsAngles * 2.0 - 1.0;
        for (std::uint32_t column = 0; column < kMsHeights; ++column) {
            double const height = (static_cast<double>(column) + 0.5) / kMsHeights * body.atmosphere_top_m;
            auto const psi = multipleScatteringAt(body, texels, height, mu);
            std::size_t const index = (static_cast<std::size_t>(row) * kMsHeights + column) * 4U;
            ms_texels[index] = static_cast<float>(psi[0]);
            ms_texels[index + 1] = static_cast<float>(psi[1]);
            ms_texels[index + 2] = static_cast<float>(psi[2]);
            ms_texels[index + 3] = 1.0f;
        }
    }
    {
        double sum = 0.0;
        double peak = 0.0;
        for (std::size_t i = 0; i < ms_texels.size(); i += 4) {
            sum += ms_texels[i + 1];
            peak = std::max<double>(peak, ms_texels[i + 1]);
        }
        std::printf("DeepSkyLab: planet multiple-scattering table, green channel: mean %.4g, peak %.4g\n",
                    sum / static_cast<double>(ms_texels.size() / 4), peak);
    }
    Diligent::TextureDesc ms_desc;
    ms_desc.Name = "Helios planet multiple-scattering table";
    ms_desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
    ms_desc.Width = kMsHeights;
    ms_desc.Height = kMsAngles;
    ms_desc.MipLevels = 1;
    ms_desc.Format = Diligent::TEX_FORMAT_RGBA32_FLOAT;
    ms_desc.Usage = Diligent::USAGE_IMMUTABLE;
    ms_desc.BindFlags = Diligent::BIND_SHADER_RESOURCE;
    Diligent::TextureSubResData ms_level;
    ms_level.pData = ms_texels.data();
    ms_level.Stride = static_cast<Diligent::Uint64>(kMsHeights) * 4U * sizeof(float);
    Diligent::TextureData ms_data;
    ms_data.pSubResources = &ms_level;
    ms_data.NumSubresources = 1;
    multiscatter_.Release();
    device_->CreateTexture(ms_desc, &ms_data, &multiscatter_);
    if (multiscatter_ == nullptr) {
        error = "Diligent failed to create the planet multiple-scattering table";
        return false;
    }
    multiscatter_->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE)->SetSampler(linear_sampler_);
    transmittance_body_ = body;
    return true;
}

bool PlanetStage::ensureRingProfile(PlanetSystem const& system, std::string& error) {
    std::vector<float> values = planetRingOpticalDepth(system, kRingSamples);
    if (ring_profile_ != nullptr && values == ring_profile_values_) {
        return true;
    }
    Diligent::TextureDesc desc;
    desc.Name = "Helios planet ring profile";
    desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
    desc.Width = static_cast<Diligent::Uint32>(values.size());
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.Format = Diligent::TEX_FORMAT_R32_FLOAT;
    desc.Usage = Diligent::USAGE_IMMUTABLE;
    desc.BindFlags = Diligent::BIND_SHADER_RESOURCE;
    Diligent::TextureSubResData level;
    level.pData = values.data();
    level.Stride = static_cast<Diligent::Uint64>(values.size()) * sizeof(float);
    Diligent::TextureData data;
    data.pSubResources = &level;
    data.NumSubresources = 1;
    ring_profile_.Release();
    device_->CreateTexture(desc, &data, &ring_profile_);
    if (ring_profile_ == nullptr) {
        error = "Diligent failed to create the planet ring profile";
        return false;
    }
    ring_profile_->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE)->SetSampler(linear_sampler_);
    ring_profile_values_ = std::move(values);
    return true;
}

bool PlanetStage::drawBody(Body& body, PlanetPatchView const& view, PlanetPatchSettings const& patches,
                           bool settle, PlanetPatchMetrics& metrics, std::string& error) {
    auto const begin = std::chrono::steady_clock::now();
    planet::LodViewInput lod_view;
    lod_view.camera_position_body_fixed_m = view.camera_body_fixed_m;
    lod_view.viewport_height_px = view.viewport_height_px;
    lod_view.vertical_fov_rad = view.vertical_fov_rad;
    // The selector keeps hysteresis state between frames.
    for (int pass = 0; pass < (settle ? kSettlePasses : 1); ++pass) {
        auto selected = body.selector->select(lod_view);
        if (selected.failed()) {
            error = "planet LOD selection: " + selected.error().to_string();
            return false;
        }
        body.selection = std::move(selected.value());
        if (pass > 0 && body.selection.splits_from_previous == 0 && body.selection.merges_from_previous == 0) {
            break;
        }
    }
    selection_ms_ +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    return renderer_.render(body.selection.leaves, view, patches, metrics, error);
}

bool PlanetStage::render(PlanetStageSettings const& settings, bool settle, std::uint32_t frame_index,
                         float star_intensity, float band_intensity, Diligent::ITextureView* output,
                         std::uint32_t width, std::uint32_t height, PlanetStageMetrics& metrics,
                         std::string& error) {
    metrics = {};
    selection_ms_ = 0.0;
    if (output == nullptr) {
        error = "the planet stage has no output";
        return false;
    }
    PlanetSystem const& system = settings.system;
    PlanetBody const& body = system.body;
    if (!ensureTargets(width, height, error) || !ensureAtmosphereTables(body, error) ||
        !ensureRingProfile(system, error) || !ensureSelector(planet_, body.radius_m, settings.lod_error_px, error)) {
        return false;
    }

    // The sun, carried westwards by the planet's rotation.
    double const rotation = system.day_length_s > 0.0
                                ? 360.0 * settings.time_s / system.day_length_s
                                : 0.0;
    double const sun_latitude = settings.sun_latitude_deg * kRadians;
    double const sun_longitude = (settings.sun_longitude_deg - rotation) * kRadians;
    math::Vec3d const to_sun{std::cos(sun_latitude) * std::cos(sun_longitude),
                             std::cos(sun_latitude) * std::sin(sun_longitude), std::sin(sun_latitude)};

    // ---- the planet, into its own colour and depth ------------------------
    PlanetPatchSettings patches = settings.patches;
    patches.body = body;
    patches.planet_radius_m = body.radius_m;
    patches.relief_amplitude_m = static_cast<float>(
        std::max(body.platform_m + body.base_roughness_m * 1.5 + body.mountain_amplitude_m + body.detail_amplitude_m,
                 1000.0));
    patches.tone_map_output = false; // the lab's post chain tone-maps
    patches.light_direction = to_sun;
    patches.time_s = settings.time_s;
    patches.ambient = body.atmosphere ? 0.18f : 0.03f;
    patches.ambient_color = ambientColor(body);
    bool const rings = system.rings.enabled && system.rings.outer_m > system.rings.inner_m;
    patches.ring_inner_m = rings ? system.rings.inner_m : 0.0;
    patches.ring_outer_m = rings ? system.rings.outer_m : 0.0;
    patches.ring_profile = rings ? ring_profile_->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
    bool const clouds = system.clouds.enabled && !body.gas_giant && system.clouds.cover > 0.0 &&
                        system.clouds.top_m > system.clouds.base_m;
    patches.cloud_base_m = clouds ? body.radius_m + system.clouds.base_m : 0.0;
    patches.cloud_top_m = clouds ? body.radius_m + system.clouds.top_m : 0.0;
    patches.cloud_cover = clouds ? system.clouds.cover : 0.0;
    patches.cloud_extinction = clouds ? system.clouds.extinction_per_m : 0.0;
    double const cloud_drift = settings.time_s * system.clouds.wind_rad_per_s;
    patches.cloud_drift_rad = cloud_drift;
    PlanetPatchView view = makePlanetView(settings.camera, patches,
                                          static_cast<double>(settings.vertical_fov_deg), width, height);
    view.jitter_px = frameJitter(frame_index);

    Diligent::ITextureView* color_target = color_->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET);
    Diligent::ITextureView* const depth = depth_->GetDefaultView(Diligent::TEXTURE_VIEW_DEPTH_STENCIL);
    context_->SetRenderTargets(1, &color_target, depth, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    float const black[4]{0.0f, 0.0f, 0.0f, 0.0f};
    context_->ClearRenderTarget(color_target, black, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    // Reversed depth: the far plane is zero.
    context_->ClearDepthStencil(depth, Diligent::CLEAR_DEPTH_FLAG, 0.0f, 0,
                                Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Diligent::Viewport const viewport{0.0f, 0.0f, static_cast<float>(width),
                                      static_cast<float>(height), 0.0f, 1.0f};
    context_->SetViewports(1, &viewport, width, height);
    if (!drawBody(planet_, view, patches, settle, metrics.patches, error)) {
        return false;
    }
    metrics.leaves = static_cast<std::uint32_t>(planet_.selection.leaves.size());
    metrics.max_error_px = planet_.selection.max_leaf_sse_px;

    // ---- the moons: each in its own frame, tidally locked ------------------
    for (std::uint32_t m = 0; m < std::min<std::uint32_t>(system.moon_count, kMaxPlanetMoons); ++m) {
        PlanetMoon const& moon = system.moons[m];
        if (moon.orbit_radius_m <= 0.0 || moon.body.radius_m <= 0.0) {
            continue;
        }
        Body& slot = moons_[m];
        if (!ensureSelector(slot, moon.body.radius_m, settings.lod_error_px, error)) {
            return false;
        }
        double const angle = moonOrbitalAngle(system, moon, settings.time_s);
        math::Vec3d const position{moon.orbit_radius_m * std::cos(angle), moon.orbit_radius_m * std::sin(angle), 0.0};
        // The moon frame: the planet frame turned so that the moon's -x axis
        // keeps facing the planet.
        PlanetPatchView moon_view = view;
        moon_view.camera_body_fixed_m = rotateZ(view.camera_body_fixed_m - position, -angle);
        moon_view.forward_body_fixed = rotateZ(view.forward_body_fixed, -angle);
        moon_view.right_body_fixed = rotateZ(view.right_body_fixed, -angle);
        moon_view.up_body_fixed = rotateZ(view.up_body_fixed, -angle);
        PlanetPatchSettings moon_patches = settings.patches;
        moon_patches.body = moon.body;
        moon_patches.planet_radius_m = moon.body.radius_m;
        moon_patches.relief_amplitude_m = static_cast<float>(
            std::max(moon.body.platform_m + moon.body.base_roughness_m * 1.5 + moon.body.mountain_amplitude_m +
                         moon.body.detail_amplitude_m,
                     moon.body.radius_m * 0.02));
        moon_patches.tone_map_output = false;
        moon_patches.light_direction = rotateZ(to_sun, -angle);
        moon_patches.time_s = settings.time_s;
        moon_patches.ambient = 0.02f; // no sky; a little light from the planet
        moon_patches.ambient_color = {1.0, 1.0, 1.0};
        // The planet may shadow the moon.
        moon_patches.occluder_centre_m = rotateZ(position * -1.0, -angle);
        moon_patches.occluder_radius_m = body.radius_m;
        PlanetPatchMetrics moon_metrics;
        if (!drawBody(slot, moon_view, moon_patches, settle, moon_metrics, error)) {
            return false;
        }
        metrics.leaves += static_cast<std::uint32_t>(slot.selection.leaves.size());
        metrics.max_error_px = std::max(metrics.max_error_px, slot.selection.max_leaf_sse_px);
        ++metrics.moons_drawn;
    }
    metrics.selection_ms = selection_ms_;

    // ---- the sky over everything, into the lab's target --------------------
    shaders::DeepSkyFrame frame{};
    double const tangent = std::tan(view.vertical_fov_rad * 0.5);
    double const aspect = static_cast<double>(width) / static_cast<double>(height);
    set(frame.camera_position, 0.0, 0.0, 0.0, settings.time_s);
    set(frame.camera_right, view.right_body_fixed[0] * tangent * aspect,
        view.right_body_fixed[1] * tangent * aspect, view.right_body_fixed[2] * tangent * aspect, 0.0);
    set(frame.camera_up, view.up_body_fixed[0] * tangent, view.up_body_fixed[1] * tangent,
        view.up_body_fixed[2] * tangent, 0.0);
    set(frame.camera_forward, view.forward_body_fixed[0], view.forward_body_fixed[1],
        view.forward_body_fixed[2], 0.0);
    set(frame.viewport, width, height, 1.0 / width, 1.0 / height);
    double const log_minimum = std::log(blackbody_.minKelvin);
    double const log_maximum = std::log(blackbody_.maxKelvin);
    set(frame.radiometry, 0.0, log_minimum, 1.0 / (log_maximum - log_minimum), blackbody_.count);
    set(frame.sky, std::max(star_intensity, 0.0f), std::max(band_intensity, 0.0f), cloud_drift,
        body.gas_giant ? 1.0 : 0.0);
    set(frame.quality, std::clamp(settings.atmosphere_samples, 4, 64), frame_index,
        std::clamp(settings.cloud_samples, 4, 96), 0.0);
    double const camera_distance = math::length(view.camera_body_fixed_m);
    math::Vec3d const centre = view.camera_body_fixed_m * -1.0;
    set(frame.object[0], centre[0], centre[1], centre[2], camera_distance);
    set(frame.object[1], body.radius_m, body.radius_m + body.atmosphere_top_m,
        camera_distance - body.radius_m, body.atmosphere ? 1.0 : 0.0);
    set(frame.object[2], to_sun[0], to_sun[1], to_sun[2], kSunAngularRadius);
    set(frame.object[3], body.rayleigh_beta[0], body.rayleigh_beta[1], body.rayleigh_beta[2],
        body.rayleigh_height_m);
    set(frame.object[4], body.mie_beta[0], body.mie_beta[1], body.mie_beta[2], body.mie_height_m);
    set(frame.object[5], body.mie_g, 1.0, kStarScale, body.atmosphere_top_m);
    set(frame.object[6], patches.ring_inner_m, patches.ring_outer_m, system.rings.albedo, rings ? 1.0 : 0.0);
    set(frame.object[7], patches.cloud_base_m, patches.cloud_top_m, patches.cloud_cover,
        patches.cloud_extinction);
    {
        Diligent::MapHelper<shaders::DeepSkyFrame> mapped{context_, frame_buffer_, Diligent::MAP_WRITE,
                                                          Diligent::MAP_FLAG_DISCARD};
        if (static_cast<shaders::DeepSkyFrame*>(mapped) == nullptr) {
            error = "failed to map the planet sky frame constants";
            return false;
        }
        *mapped = frame;
    }
    {
        Diligent::MapHelper<shaders::PlanetBodyParams> mapped{context_, body_buffer_, Diligent::MAP_WRITE,
                                                              Diligent::MAP_FLAG_DISCARD};
        if (static_cast<shaders::PlanetBodyParams*>(mapped) == nullptr) {
            error = "failed to map the planet sky body constants";
            return false;
        }
        *mapped = planetBodyParams(body);
    }
    {
        Diligent::MapHelper<shaders::PlanetSkyExtra> mapped{context_, extra_buffer_, Diligent::MAP_WRITE,
                                                            Diligent::MAP_FLAG_DISCARD};
        if (static_cast<shaders::PlanetSkyExtra*>(mapped) == nullptr) {
            error = "failed to map the planet sky extra constants";
            return false;
        }
        set(mapped->ring_tint, system.rings.tint[0], system.rings.tint[1], system.rings.tint[2], 0.0);
    }
    if (!bindVariable(sky_resources_, "terrain_depth",
                      depth_->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE), error) ||
        !bindVariable(sky_resources_, "terrain_color",
                      color_->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE), error) ||
        !bindVariable(sky_resources_, "transmittance_lut",
                      transmittance_->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE), error) ||
        !bindVariable(sky_resources_, "ring_profile",
                      ring_profile_->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE), error) ||
        !bindVariable(sky_resources_, "multiscatter_lut",
                      multiscatter_->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE), error)) {
        return false;
    }
    drawFullscreen(context_, {&output, 1}, width, height, sky_pipeline_, sky_resources_);
    return true;
}

} // namespace helios::examples

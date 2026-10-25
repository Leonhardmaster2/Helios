// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "deep_sky_renderer.hpp"

#include "MapHelper.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace helios::examples {
namespace {

constexpr double kTableMinimumKelvin = 500.0;
constexpr double kTableMaximumKelvin = 1.0e6;
constexpr std::uint32_t kBlackbodyTableSize = 512;

constexpr double kRadiansPerDegree = std::numbers::pi / 180.0;

// Display scale at 0 EV for each object, chosen so the default view sits
// around display white before the tone map.
// (The black hole and the planet have their own display chains.)
// The planet's sunlit ground has a radiance of about albedo / pi, so its
// white sits near 0.3.
constexpr std::array<float, kDeepSkyObjectCount> kBaseExposure{1.0f, 2.2f, 1.0f, 1.0f,
                                                                1.0f, 3.5f, 2.2f};

// The black hole's disk pattern is periodic in this many gravitational
// times; wrapping the clock keeps its float32 time precise in long sessions.
constexpr double kDiskTimeWrapInMasses = 180.0 * 1024.0;

// Emissivities of the neutron star's non-thermal components, in surface
// radiances per M, at unit brightness setting.  The outflow and the wind are
// normalised to the beaming they would show with a Lorentz factor of 2, so
// that a brightness setting means the same at any Lorentz factor.
constexpr double kBeamEmissivity = 0.08;
constexpr double kTwistEmissivity = 0.02;
// The sheet's is per light-cylinder radius: its path length scales with it.
constexpr double kSheetEmissivity = 0.3;
// The streams' is per (r_m * width): their path length.
constexpr double kStreamEmissivity = 0.5;
// Photon energies of the 0.2-10 keV band divided by those of visible light:
// a black body at T looks in X-rays as one at T / kXRayBandFactor does to
// the eye.
constexpr double kXRayBandFactor = 1.0 / 435.0;

// D^(2.5) of a flow with this Lorentz factor pointing at the camera.
[[nodiscard]] double onAxisBeaming(double lorentz) noexcept {
    double const beta = std::sqrt(std::max(1.0 - 1.0 / (lorentz * lorentz), 0.0));
    return std::pow(lorentz * (1.0 + beta), 2.5);
}
// Peak radiance of a star's image per solar luminosity (photopic) at the
// reference distance, and of the remnant's pulsar.
constexpr double kStarPeak = 5.0e-3;
constexpr double kPulsarPeak = 1.5;
// Stars are scaled by the inverse square of their distance relative to this
// many half extents of the gas volume.
constexpr double kReferenceDistance = 3.25;

void store(float destination[4], math::Vec3d const& value, double scale, double w) {
    destination[0] = static_cast<float>(value[0] * scale);
    destination[1] = static_cast<float>(value[1] * scale);
    destination[2] = static_cast<float>(value[2] * scale);
    destination[3] = static_cast<float>(w);
}

void set(float destination[4], double x, double y, double z, double w) {
    destination[0] = static_cast<float>(x);
    destination[1] = static_cast<float>(y);
    destination[2] = static_cast<float>(z);
    destination[3] = static_cast<float>(w);
}

[[nodiscard]] bool finiteVector(math::Vec3d const& value) noexcept {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

[[nodiscard]] bool sameVector(math::Vec3d const& a, math::Vec3d const& b) noexcept {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

[[nodiscard]] bool sameMoon(PlanetMoon const& a, PlanetMoon const& b) noexcept {
    return a.body == b.body && a.orbit_radius_m == b.orbit_radius_m && a.mass_ratio == b.mass_ratio &&
           a.phase_rad == b.phase_rad;
}

[[nodiscard]] bool samePlanetSystem(PlanetSystem const& a, PlanetSystem const& b) noexcept {
    if (!(a.body == b.body) || a.mean_density_kg_m3 != b.mean_density_kg_m3 ||
        a.day_length_s != b.day_length_s || a.moon_count != b.moon_count) {
        return false;
    }
    PlanetRings const& r = a.rings;
    PlanetRings const& s = b.rings;
    if (r.enabled != s.enabled || r.inner_m != s.inner_m || r.outer_m != s.outer_m ||
        r.peak_optical_depth != s.peak_optical_depth || r.albedo != s.albedo || !sameVector(r.tint, s.tint) ||
        r.ringlet_contrast != s.ringlet_contrast || r.seed != s.seed) {
        return false;
    }
    PlanetClouds const& c = a.clouds;
    PlanetClouds const& d = b.clouds;
    if (c.enabled != d.enabled || c.cover != d.cover || c.base_m != d.base_m || c.top_m != d.top_m ||
        c.extinction_per_m != d.extinction_per_m || c.wind_rad_per_s != d.wind_rad_per_s || c.seed != d.seed) {
        return false;
    }
    for (std::uint32_t m = 0; m < std::min<std::uint32_t>(a.moon_count, kMaxPlanetMoons); ++m) {
        if (!sameMoon(a.moons[m], b.moons[m])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool samePlanet(PlanetStageSettings const& a, PlanetStageSettings const& b) noexcept {
    return samePlanetSystem(a.system, b.system) && a.time_s == b.time_s &&
           a.atmosphere_samples == b.atmosphere_samples && a.cloud_samples == b.cloud_samples &&
           a.camera.altitude_agl_m == b.camera.altitude_agl_m &&
           a.camera.latitude_deg == b.camera.latitude_deg &&
           a.camera.longitude_deg == b.camera.longitude_deg &&
           a.camera.look_down_deg == b.camera.look_down_deg &&
           a.vertical_fov_deg == b.vertical_fov_deg && a.lod_error_px == b.lod_error_px &&
           a.sun_latitude_deg == b.sun_latitude_deg && a.sun_longitude_deg == b.sun_longitude_deg &&
           a.patches.body == b.patches.body && a.patches.debug_color == b.patches.debug_color &&
           a.patches.wire_overlay == b.patches.wire_overlay;
}

// True when two settings produce the same radiance, so their frames may be
// averaged.  Display controls are applied after accumulation and do not count.
[[nodiscard]] bool sameRadiance(DeepSkySettings const& a, DeepSkySettings const& b) noexcept {
    return a.object == b.object && sameVector(a.camera.position, b.camera.position) &&
           sameVector(a.camera.forward, b.camera.forward) &&
           sameVector(a.camera.right, b.camera.right) && sameVector(a.camera.up, b.camera.up) &&
           a.camera.vertical_fov_rad == b.camera.vertical_fov_rad && a.time == b.time &&
           a.nebula == b.nebula && a.neutron_star == b.neutron_star && a.pulsar == b.pulsar &&
           a.galaxy == b.galaxy &&
           a.remnant == b.remnant && a.star_intensity == b.star_intensity &&
           a.band_intensity == b.band_intensity && a.steps == b.steps &&
           a.step_scale == b.step_scale && samePlanet(a.planet, b.planet);
}

} // namespace

bool DeepSkyRenderer::initialize(Diligent::IRenderDevice* device,
                                 Diligent::IDeviceContext* context,
                                 Diligent::TEXTURE_FORMAT output_format, std::uint32_t threads,
                                 std::string& error) {
    if (device == nullptr || context == nullptr) {
        error = "deep-sky renderer requires a valid Diligent device and context";
        return false;
    }
    if (device->GetDeviceInfo().Type != Diligent::RENDER_DEVICE_TYPE_VULKAN) {
        error = "deep-sky renderer is a Vulkan-only v0.0.1 path";
        return false;
    }
    device_ = device;
    context_ = context;
    threads_ = std::max(1U, threads);

    frame_buffer_ =
        createDynamicUniformBuffer(device_, "Helios deep-sky frame constants", sizeof(FrameGpu));
    sources_buffer_ = createDynamicUniformBuffer(device_, "Helios deep-sky point sources",
                                                 sizeof(shaders::PointSourcesBlock));
    if (frame_buffer_ == nullptr || sources_buffer_ == nullptr) {
        error = "Diligent failed to create the deep-sky constant buffers";
        return false;
    }

    vertex_shader_ = createEmbeddedShader(device_, "fullscreen.slang.vertex.spv",
                                          "Helios full-screen vertex shader",
                                          Diligent::SHADER_TYPE_VERTEX, error);
    if (vertex_shader_ == nullptr || !createTextures(error) || !createPipelines(error) ||
        !galaxy_.initialize(device_, context_, frame_buffer_, Diligent::TEX_FORMAT_RGBA16_FLOAT,
                            blackbody_texture_->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE),
                            blackbody_table_, error) ||
        !nebula_.initialize(device_, context_, error) ||
        !remnant_.initialize(device_, context_, error) ||
        !post_.initialize(device_, context_, vertex_shader_, output_format, error)) {
        return false;
    }
    BlackHoleRenderer::CreateInfo black_hole_info;
    black_hole_info.output_format = output_format;
    // The planet draws into the lab's HDR scene target and goes through the
    // same accumulation and display chain as the other objects.
    if (!black_hole_.initialize(device_, context_, black_hole_info, error) ||
        !planet_.initialize(device_, context_, Diligent::TEX_FORMAT_RGBA16_FLOAT,
                            blackbody_texture_->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE),
                            blackbody_table_, error)) {
        return false;
    }
    scene_timer_.initialize(device_, "Helios deep-sky scene duration");
    post_timer_.initialize(device_, "Helios deep-sky display duration");
    simulation_timer_.initialize(device_, "Helios deep-sky simulation duration");
    return true;
}

bool DeepSkyRenderer::createTextures(std::string& error) {
    blackbody_table_ = blackhole::makeBlackbodyTable(kTableMinimumKelvin, kTableMaximumKelvin,
                                                     kBlackbodyTableSize);
    if (!blackbody_table_.valid()) {
        error = "failed to build the black-body colour table";
        return false;
    }
    Diligent::TextureDesc table_desc;
    table_desc.Name = "Helios black-body colour table";
    table_desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
    table_desc.Width = blackbody_table_.count;
    table_desc.Height = 1;
    table_desc.MipLevels = 1;
    table_desc.Format = Diligent::TEX_FORMAT_RGBA32_FLOAT;
    table_desc.Usage = Diligent::USAGE_IMMUTABLE;
    table_desc.BindFlags = Diligent::BIND_SHADER_RESOURCE;
    Diligent::TextureSubResData table_level;
    table_level.pData = blackbody_table_.rgba.data();
    table_level.Stride = static_cast<Diligent::Uint64>(blackbody_table_.count) * 4U * sizeof(float);
    Diligent::TextureData table_data;
    table_data.pSubResources = &table_level;
    table_data.NumSubresources = 1;
    device_->CreateTexture(table_desc, &table_data, &blackbody_texture_);
    source_light_ = createRenderTexture(device_, "Helios deep-sky point-source transmittance",
                                        static_cast<std::uint32_t>(
                                            shaders::PointSourcesBlock::kMaximumSources),
                                        1, Diligent::TEX_FORMAT_RGBA16_FLOAT, nullptr);
    if (blackbody_texture_ == nullptr || source_light_ == nullptr) {
        error = "Diligent failed to create the deep-sky lookup textures";
        return false;
    }
    return true;
}

bool DeepSkyRenderer::createScenePass(Pass& pass, char const* shader, char const* name,
                                      char const* dynamic_texture, bool sources,
                                      Diligent::ITexture* source_light, std::string& error) {
    constexpr std::array kSceneFormat{Diligent::TEX_FORMAT_RGBA16_FLOAT};
    auto module = createEmbeddedShader(device_, shader, name, Diligent::SHADER_TYPE_PIXEL, error);
    if (module == nullptr) {
        return false;
    }
    // A texture that changes between draws: the gas volume differs between
    // the nebula and the remnant, and the galaxy's half-resolution light is
    // recreated with the scene targets.
    Diligent::ShaderResourceVariableDesc const dynamic_variables[] = {
        {Diligent::SHADER_TYPE_PIXEL, dynamic_texture != nullptr ? dynamic_texture : "",
         Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC}};
    FullscreenPipelineInfo info;
    info.name = name;
    info.vertex_shader = vertex_shader_;
    info.pixel_shader = module;
    info.target_formats = kSceneFormat;
    if (dynamic_texture != nullptr) {
        info.variables = dynamic_variables;
    }
    pass.pipeline = createFullscreenPipeline(device_, info);
    if (pass.pipeline == nullptr) {
        error = std::string{"Diligent failed to create "} + name;
        return false;
    }
    if (!bindStaticVariable(pass.pipeline, "frame", frame_buffer_, error) ||
        (sources && !bindStaticVariable(pass.pipeline, "sources", sources_buffer_, error)) ||
        (source_light != nullptr &&
         !bindStaticVariable(pass.pipeline, "source_light",
                             source_light->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE),
                             error))) {
        return false;
    }
    // A pass that draws no black body does not declare the table.
    if (auto* const table = pass.pipeline->GetStaticVariableByName(Diligent::SHADER_TYPE_PIXEL,
                                                                   "blackbody_table")) {
        table->Set(blackbody_texture_->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
    }
    pass.pipeline->CreateShaderResourceBinding(&pass.resources, true);
    if (pass.resources == nullptr) {
        error = "Diligent failed to create a deep-sky resource binding";
        return false;
    }
    return true;
}

bool DeepSkyRenderer::createPipelines(std::string& error) {
    if (!createScenePass(neutron_star_, "neutron_star.slang.fragment.spv",
                         "Helios deep-sky neutron-star pipeline", nullptr, false, nullptr, error) ||
        !createScenePass(background_, "deep_sky_background.slang.fragment.spv",
                         "Helios deep-sky background pipeline", "galaxy_light", false, nullptr,
                         error) ||
        !createScenePass(volume_, "emission_volume.slang.fragment.spv",
                         "Helios deep-sky gas volume pipeline", "emission_volume", true,
                         source_light_, error) ||
        !createScenePass(source_dust_, "point_source_dust.slang.fragment.spv",
                         "Helios deep-sky point-source dust pipeline", "emission_volume", true,
                         nullptr, error)) {
        return false;
    }

    constexpr std::array kSceneFormat{Diligent::TEX_FORMAT_RGBA16_FLOAT};
    auto accumulate_shader =
        createEmbeddedShader(device_, "hdr_accumulate.slang.fragment.spv",
                             "Helios HDR accumulation shader", Diligent::SHADER_TYPE_PIXEL, error);
    if (accumulate_shader == nullptr) {
        return false;
    }
    Diligent::ShaderResourceVariableDesc const accumulate_variables[] = {
        {Diligent::SHADER_TYPE_PIXEL, "source", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE}};
    FullscreenPipelineInfo accumulate_info;
    accumulate_info.name = "Helios HDR accumulation pipeline";
    accumulate_info.vertex_shader = vertex_shader_;
    accumulate_info.pixel_shader = accumulate_shader;
    accumulate_info.target_formats = kSceneFormat;
    accumulate_info.blend = FullscreenBlend::ConstantFactor;
    accumulate_info.variables = accumulate_variables;
    accumulate_pipeline_ = createFullscreenPipeline(device_, accumulate_info);
    if (accumulate_pipeline_ == nullptr) {
        error = "Diligent failed to create the HDR accumulation pipeline";
        return false;
    }
    return true;
}

bool DeepSkyRenderer::ensureTargets(std::uint32_t width, std::uint32_t height,
                                    std::string& error) {
    if (scene_ != nullptr && width == scene_width_ && height == scene_height_) {
        return true;
    }
    accumulate_resources_.Release();
    scene_ = createRenderTexture(device_, "Helios deep-sky frame radiance", width, height,
                                 Diligent::TEX_FORMAT_RGBA16_FLOAT, post_.linearSampler());
    accumulated_ = createRenderTexture(device_, "Helios deep-sky accumulated radiance", width,
                                       height, Diligent::TEX_FORMAT_RGBA16_FLOAT,
                                       post_.linearSampler());
    galaxy_light_ = createRenderTexture(device_, "Helios deep-sky galaxy old-star light",
                                        std::max(width / 2U, 1U), std::max(height / 2U, 1U),
                                        Diligent::TEX_FORMAT_RGBA16_FLOAT, post_.linearSampler());
    if (scene_ == nullptr || accumulated_ == nullptr || galaxy_light_ == nullptr) {
        error = "Diligent failed to create the deep-sky scene targets";
        return false;
    }
    accumulate_pipeline_->CreateShaderResourceBinding(&accumulate_resources_, true);
    if (!bindVariable(accumulate_resources_, "source",
                      scene_->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE), error)) {
        return false;
    }
    scene_width_ = width;
    scene_height_ = height;
    accumulated_frames_ = 0;
    return true;
}

GasScene const& DeepSkyRenderer::gasScene(DeepSkyObject object) const {
    return object == DeepSkyObject::SupernovaRemnant ? remnant_ : nebula_;
}

DeepSkyRenderer::FrameGpu DeepSkyRenderer::makeFrame(DeepSkySettings const& settings,
                                                     std::uint32_t width,
                                                     std::uint32_t height) const {
    FrameGpu frame{};
    DeepSkyCamera const& camera = settings.camera;
    double const tangent = std::tan(camera.vertical_fov_rad * 0.5);
    double const aspect = static_cast<double>(width) / static_cast<double>(height);
    store(frame.camera_position, camera.position, 1.0, settings.time);
    store(frame.camera_right, camera.right, tangent * aspect, 0.0);
    store(frame.camera_up, camera.up, tangent, 0.0);
    store(frame.camera_forward, camera.forward, 1.0, 0.0);
    set(frame.viewport, width, height, 1.0 / width, 1.0 / height);

    double const log_minimum = std::log(blackbody_table_.minKelvin);
    double const log_maximum = std::log(blackbody_table_.maxKelvin);
    set(frame.radiometry, 0.0, log_minimum, 1.0 / (log_maximum - log_minimum),
        blackbody_table_.count);
    set(frame.sky, std::max(settings.star_intensity, 0.0f), std::max(settings.band_intensity, 0.0f),
        0.0, 0.0);
    set(frame.quality, std::clamp(settings.steps, 8, 512), accumulated_frames_,
        std::clamp(settings.step_scale, 0.25f, 4.0f), 0.0);

    switch (settings.object) {
    case DeepSkyObject::Nebula:
    case DeepSkyObject::SupernovaRemnant: {
        GasScene const& gas = gasScene(settings.object);
        bool const nebula = settings.object == DeepSkyObject::Nebula;
        double const emission = nebula ? std::max(settings.nebula.emission, 0.0f) : 1.0;
        double const dust =
            nebula ? std::max(settings.nebula.dust_opacity, 0.0f) : std::max(settings.remnant.dust, 0.0f);
        std::size_t const count =
            std::min(gas.sources().size(), shaders::PointSourcesBlock::kMaximumSources);
        set(frame.object[0], gas.halfExtent(), emission, gas.dustOpacity() * dust,
            static_cast<double>(count));
        set(frame.object[1], 0.52, gas.stretch(), 0.0, 0.0);
        math::Vec3d const scatter = gas.ambientScatter() * emission;
        bool const continues = scatter[0] > 0.0 || scatter[1] > 0.0 || scatter[2] > 0.0;
        set(frame.object[2], scatter[0], scatter[1], scatter[2], continues ? 1.0 : 0.0);
        store(frame.object[3], gas.starPosition(), 1.0, 0.0);
        break;
    }
    case DeepSkyObject::NeutronStar:
    case DeepSkyObject::Pulsar: {
        NeutronStarSettings const& star =
            settings.object == DeepSkyObject::Pulsar ? settings.pulsar : settings.neutron_star;
        NeutronStarDerived const d = deriveNeutronStar(star);
        double const radius = std::clamp(static_cast<double>(star.radius_in_masses), 3.2, 12.0);
        double const phase =
            d.omega > 0.0 ? std::remainder(d.omega * settings.time, 2.0 * std::numbers::pi) : 0.0;
        bool const xray = star.band == NeutronStarBand::XRay;
        double const band = xray ? kXRayBandFactor : 1.0;
        double const lorentz = std::clamp(static_cast<double>(star.lorentz_factor), 1.0, 100.0);
        double const wind_lorentz =
            std::clamp(static_cast<double>(star.wind_lorentz_factor), 1.0, 100.0);
        double const reference_beaming = onAxisBeaming(2.0);
        set(frame.object[0], radius, d.omega, star.magnetic_inclination_deg * kRadiansPerDegree,
            phase);
        set(frame.object[1], star.polar_temperature_k, star.cap_temperature_k, star.grid,
            std::clamp(static_cast<double>(star.conductivity_ratio), 1.0e-4, 1.0));
        set(frame.object[2],
            kBeamEmissivity * std::max(star.outflow, 0.0f) * reference_beaming / onAxisBeaming(lorentz),
            lorentz, d.light_cylinder, d.cap_reach);
        set(frame.object[3], kTwistEmissivity * std::max(star.twist_glow, 0.0f),
            static_cast<double>(star.twist_reach_radii) * radius,
            d.sheet_outer > 0.0 ? kSheetEmissivity / d.light_cylinder * std::max(star.wind_sheet, 0.0f) *
                                      reference_beaming / onAxisBeaming(wind_lorentz)
                                : 0.0,
            wind_lorentz);
        double const stream_width = std::clamp(static_cast<double>(star.stream_width), 0.02, 1.0);
        bool const streams = star.accreting && !d.propeller && star.stream_brightness > 0.0f;
        set(frame.object[4], d.magnetospheric_radius, d.disk_outer_radius, stream_width,
            streams ? kStreamEmissivity * star.stream_brightness / (d.magnetospheric_radius * stream_width)
                    : 0.0);
        set(frame.object[5], d.disk_temperature_scale_k, d.spot_temperature_k, band, d.sheet_outer);
        set(frame.object[6], d.emission_outer, d.stream_launch_speed, streams ? 1.0 : 0.0,
            d.irradiation_k4_m2);
        // Display white: the polar surface in visible light (or the disk, if
        // hotter); in X-rays the hottest thing there is.
        double reference = static_cast<double>(star.polar_temperature_k);
        if (star.accreting) {
            reference = std::max(reference, d.inner_disk_temperature_k);
        }
        if (xray) {
            // Display white stays the surface (or the disk), so that it is
            // seen beside caps and spots far hotter than it, which the tone
            // map lets clip; nothing cooler than 4e5 K shows in the band.
            reference = std::max(reference, 1000.0 / kXRayBandFactor);
        }
        double const log_reference =
            blackhole::sampleBlackbodyLogLuminance(blackbody_table_, reference * band);
        frame.radiometry[0] = static_cast<float>(log_reference);
        if (xray) {
            // Ordinary stars are dark in X-rays; the sky is kept at a quarter
            // of its visible-light brightness for orientation and to show the
            // light bending.  (The sky is in absolute units; the object is
            // relative to the reference, so the change of reference is undone.)
            double const visible_reference = blackhole::sampleBlackbodyLogLuminance(
                blackbody_table_, static_cast<double>(star.polar_temperature_k));
            auto const sky_scale = static_cast<float>(0.25 * std::exp(log_reference - visible_reference));
            frame.sky[0] *= sky_scale;
            frame.sky[1] *= sky_scale;
        }
        break;
    }
    case DeepSkyObject::Galaxy:
        galaxy_.fillFrame(frame, settings.galaxy);
        break;
    case DeepSkyObject::BlackHole:
    case DeepSkyObject::Planet:
        break; // drawn by their own renderers
    }
    return frame;
}

void DeepSkyRenderer::writeSources(DeepSkySettings const& settings, std::uint32_t width,
                                   std::uint32_t height) {
    Diligent::MapHelper<shaders::PointSourcesBlock> mapped{context_, sources_buffer_,
                                                          Diligent::MAP_WRITE,
                                                          Diligent::MAP_FLAG_DISCARD};
    GasScene const& gas = gasScene(settings.object);
    bool const nebula = settings.object == DeepSkyObject::Nebula;
    DeepSkyCamera const& camera = settings.camera;
    double const tangent = std::tan(camera.vertical_fov_rad * 0.5);
    double const aspect = static_cast<double>(width) / static_cast<double>(height);
    // Star images are sized relative to a 1080-line frame so a capture looks
    // the same at any resolution.
    double const pixel_scale = static_cast<double>(height) / 1080.0;
    double const reference_distance = kReferenceDistance * gas.halfExtent();
    double const scale = nebula ? kStarPeak * std::max(settings.nebula.star_brightness, 0.0f)
                                : kPulsarPeak * std::max(settings.remnant.pulsar, 0.0f);

    std::size_t const count =
        std::min(gas.sources().size(), shaders::PointSourcesBlock::kMaximumSources);
    for (std::size_t index = 0; index < count; ++index) {
        GasPointSource const& source = gas.sources()[index];
        float* const placement = mapped->data[3 * index];
        float* const radiance = mapped->data[3 * index + 1];
        float* const position = mapped->data[3 * index + 2];
        store(position, source.position, 1.0, 0.0);
        math::Vec3d const to_source = source.position - camera.position;
        double const depth = math::dot(to_source, camera.forward);
        if (depth <= 1.0e-3 * gas.halfExtent()) {
            set(placement, -1.0e6, -1.0e6, 1.0, 1.0);
            set(radiance, 0.0, 0.0, 0.0, 0.0);
            continue;
        }
        double const distance = math::length(to_source);
        double const peak = scale * source.luminosity * (reference_distance * reference_distance) /
                            (distance * distance);
        double const ndc_x = math::dot(to_source, camera.right) / (depth * tangent * aspect);
        double const ndc_y = math::dot(to_source, camera.up) / (depth * tangent);
        // The image is the telescope's response to a point: its width does
        // not depend on the star, the reach of its spikes on the brightness.
        set(placement, (ndc_x * 0.5 + 0.5) * width, (0.5 - 0.5 * ndc_y) * height, 1.1 * pixel_scale,
            (2.0 + 5.0 * std::sqrt(std::min(peak, 30.0))) * pixel_scale);
        math::Vec3d const chroma = blackhole::sampleBlackbodyTable(
            blackbody_table_, source.temperature_k,
            blackhole::sampleBlackbodyLogLuminance(blackbody_table_, source.temperature_k));
        set(radiance, peak * chroma[0], peak * chroma[1], peak * chroma[2], 0.0);
    }
}

bool DeepSkyRenderer::render(DeepSkySettings const& settings, std::uint32_t scene_width,
                             std::uint32_t scene_height, Diligent::ITextureView* output,
                             std::uint32_t output_width, std::uint32_t output_height,
                             DeepSkyMetrics& metrics, std::string& error) {
    metrics = {};
    metrics.gpu_timer_supported = scene_timer_.supported();
    if (accumulate_pipeline_ == nullptr ||
        static_cast<std::size_t>(settings.object) >= kDeepSkyObjectCount) {
        error = "deep-sky renderer was not initialized or received an unknown object";
        return false;
    }
    DeepSkyCamera const& camera = settings.camera;
    if (!finiteVector(camera.position) || !finiteVector(camera.forward) ||
        !finiteVector(camera.right) || !finiteVector(camera.up) ||
        !std::isfinite(camera.vertical_fov_rad) || camera.vertical_fov_rad <= 0.0 ||
        camera.vertical_fov_rad >= std::numbers::pi || !std::isfinite(settings.time) ||
        scene_width == 0 || scene_height == 0 || scene_width > 16384 || scene_height > 16384 ||
        output == nullptr || output_width == 0 || output_height == 0) {
        error = "deep-sky renderer received an invalid camera, time, resolution or output";
        return false;
    }
    if (settings.object == DeepSkyObject::NeutronStar || settings.object == DeepSkyObject::Pulsar) {
        NeutronStarSettings const& star =
            settings.object == DeepSkyObject::Pulsar ? settings.pulsar : settings.neutron_star;
        if (math::length(camera.position) <= static_cast<double>(star.radius_in_masses) * 1.05) {
            error = "the neutron-star camera must stay outside the stellar surface";
            return false;
        }
    }

    // The objects of the other labs bring their own renderers, which draw
    // straight into the output.
    if (settings.object == DeepSkyObject::BlackHole) {
        if (math::length(camera.position) < BlackHoleRenderer::kMinimumCameraRadiusInMasses) {
            error = "the black-hole camera must stay outside the horizon";
            return false;
        }
        BlackHoleSettings scene = settings.black_hole;
        scene.camera.position_in_masses = camera.position;
        scene.camera.forward = camera.forward;
        scene.camera.right = camera.right;
        scene.camera.up = camera.up;
        scene.camera.vertical_fov_rad = camera.vertical_fov_rad;
        scene.disk_time_in_masses = std::fmod(settings.time, kDiskTimeWrapInMasses);
        scene.star_intensity = settings.star_intensity;
        scene.galaxy_intensity = settings.band_intensity;
        scene.exposure_ev = settings.exposure_ev;
        scene.glare = settings.glare;
        scene.tone_map = settings.tone_map;
        scene.dither = settings.dither;
        BlackHoleMetrics black_hole;
        if (!black_hole_.render(scene, scene_width, scene_height, output, output_width,
                                output_height, black_hole, error)) {
            return false;
        }
        metrics.scene_width = black_hole.scene_width;
        metrics.scene_height = black_hole.scene_height;
        metrics.accumulated_frames = 1;
        metrics.gpu_timer_supported = black_hole.gpu_timer_supported;
        metrics.gpu_sample_new = black_hole.gpu_sample_new;
        metrics.gpu_scene_ms = black_hole.gpu_trace_ms;
        metrics.gpu_post_ms = black_hole.gpu_post_ms;
        ++frame_index_;
        return true;
    }

    // Advance whatever is simulated.  A gas run takes as many time steps as
    // fit its GPU budget, from the measured cost of a step.
    bool simulation_changed = false;
    std::size_t const timer_slot = frame_index_ % simulation_steps_.size();
    if (simulation_timer_.begin(context_, frame_index_) && simulation_steps_[timer_slot] > 0) {
        double const per_step = simulation_timer_.latestMs() / simulation_steps_[timer_slot];
        step_ms_ = step_ms_ > 0.0 ? 0.8 * step_ms_ + 0.2 * per_step : per_step;
    }
    double const budget = std::clamp(static_cast<double>(settings.simulation_budget_ms), 0.5, 100.0);
    auto const steps = static_cast<std::uint32_t>(
        std::clamp(step_ms_ > 0.0 ? budget / step_ms_ : 1.0, 1.0, 32.0));
    std::uint64_t steps_before = 0;
    switch (settings.object) {
    case DeepSkyObject::Nebula:
        steps_before = nebula_.status().steps;
        if (!nebula_.update(settings.nebula, threads_, steps, settings.wait_for_simulation,
                            simulation_changed, error)) {
            return false;
        }
        metrics.gas = nebula_.status();
        break;
    case DeepSkyObject::SupernovaRemnant:
        steps_before = remnant_.status().steps;
        if (!remnant_.update(settings.remnant, threads_, steps, settings.wait_for_simulation,
                             simulation_changed, error)) {
            return false;
        }
        metrics.gas = remnant_.status();
        break;
    case DeepSkyObject::Galaxy:
        if (!galaxy_.update(settings.galaxy, threads_, settings.wait_for_simulation,
                            simulation_changed, error)) {
            return false;
        }
        break;
    case DeepSkyObject::NeutronStar:
    case DeepSkyObject::Pulsar:
    case DeepSkyObject::BlackHole:
    case DeepSkyObject::Planet:
        break;
    }
    simulation_timer_.end(context_);
    // A restart begins counting again; it is not a negative number of steps.
    metrics.simulation_steps = metrics.gas.steps >= steps_before
                                   ? static_cast<std::uint32_t>(metrics.gas.steps - steps_before)
                                   : 0U;
    simulation_steps_[timer_slot] = metrics.simulation_steps;
    metrics.gpu_simulation_ms = simulation_timer_.latestMs();
    metrics.gpu_step_ms = step_ms_;
    if (!ensureTargets(scene_width, scene_height, error)) {
        return false;
    }
    metrics.scene_width = scene_width;
    metrics.scene_height = scene_height;

    // The galaxy's camera orbits its focus, which moves with the galaxy: the
    // view is placed about it now that the display time is known.
    DeepSkySettings const* active = &settings;
    DeepSkySettings placed;
    if (settings.object == DeepSkyObject::Galaxy) {
        placed = settings;
        double const display_exposure =
            kBaseExposure[static_cast<std::size_t>(settings.object)] * std::exp2(settings.exposure_ev);
        if (!galaxy_.prepareView(placed.camera, settings.galaxy, scene_width, scene_height, display_exposure, error)) {
            return false;
        }
        metrics.galaxy = galaxy_.status();
        active = &placed;
    }

    // A view that changed, or accumulation switched off, starts a new average.
    int const limit = std::max(settings.max_accumulation, 1);
    if (limit == 1 || simulation_changed || !sameRadiance(*active, accumulated_settings_)) {
        accumulated_frames_ = 0;
    }
    accumulated_settings_ = *active;

    FrameGpu const frame = makeFrame(*active, scene_width, scene_height);
    {
        Diligent::MapHelper<FrameGpu> mapped{context_, frame_buffer_, Diligent::MAP_WRITE,
                                             Diligent::MAP_FLAG_DISCARD};
        if (static_cast<FrameGpu*>(mapped) == nullptr) {
            error = "failed to map the deep-sky frame constant buffer";
            return false;
        }
        *mapped = frame;
    }

    bool const scene_sample = scene_timer_.begin(context_, frame_index_);
    Diligent::ITextureView* const scene_target =
        scene_->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET);
    switch (settings.object) {
    case DeepSkyObject::Nebula:
    case DeepSkyObject::SupernovaRemnant: {
        writeSources(settings, scene_width, scene_height);
        Diligent::ITextureView* const emission =
            gasScene(settings.object).emission()->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
        if (!bindVariable(volume_.resources, "emission_volume", emission, error) ||
            !bindVariable(source_dust_.resources, "emission_volume", emission, error)) {
            return false;
        }
        // The dust in front of each star, once per star ...
        Diligent::ITextureView* const light_target =
            source_light_->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET);
        drawFullscreen(context_, {&light_target, 1}, source_light_->GetDesc().Width, 1,
                       source_dust_.pipeline, source_dust_.resources);
        // ... then the gas, and the stars over it.
        drawFullscreen(context_, {&scene_target, 1}, scene_width, scene_height, volume_.pipeline,
                       volume_.resources);
        break;
    }
    case DeepSkyObject::NeutronStar:
    case DeepSkyObject::Pulsar:
        drawFullscreen(context_, {&scene_target, 1}, scene_width, scene_height,
                       neutron_star_.pipeline, neutron_star_.resources);
        break;
    case DeepSkyObject::BlackHole:
        break; // handled above
    case DeepSkyObject::Planet: {
        // The lab's time is the planet's, in seconds.
        PlanetStageSettings planet = settings.planet;
        planet.time_s = settings.time;
        if (!planet_.render(planet, settings.wait_for_simulation,
                            static_cast<std::uint32_t>(std::max(accumulated_frames_, 0)), settings.star_intensity,
                            settings.band_intensity, scene_target, scene_width, scene_height,
                            metrics.planet, error)) {
            return false;
        }
        break;
    }
    case DeepSkyObject::Galaxy: {
        // The smooth light of the old stars, at half resolution ...
        std::uint32_t const low_width = std::max(scene_width / 2U, 1U);
        std::uint32_t const low_height = std::max(scene_height / 2U, 1U);
        Diligent::ITextureView* low_target =
            galaxy_light_->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET);
        context_->SetRenderTargets(1, &low_target, nullptr,
                                   Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        float const black[4]{0.0f, 0.0f, 0.0f, 0.0f};
        context_->ClearRenderTarget(low_target, black,
                                    Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        Diligent::Viewport const viewport{0.0f, 0.0f, static_cast<float>(low_width),
                                          static_cast<float>(low_height), 0.0f, 1.0f};
        context_->SetViewports(1, &viewport, low_width, low_height);
        galaxy_.draw(makeFrame(*active, low_width, low_height), GalaxyScene::Pass::OldStars);
        // ... then the sky plus that light, and the sharp sources over it.
        {
            Diligent::MapHelper<FrameGpu> mapped{context_, frame_buffer_, Diligent::MAP_WRITE,
                                                 Diligent::MAP_FLAG_DISCARD};
            *mapped = frame;
        }
        if (!bindVariable(background_.resources, "galaxy_light",
                          galaxy_light_->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE),
                          error)) {
            return false;
        }
        drawFullscreen(context_, {&scene_target, 1}, scene_width, scene_height, background_.pipeline,
                       background_.resources);
        galaxy_.draw(frame, GalaxyScene::Pass::YoungStars);
        galaxy_.draw(frame, GalaxyScene::Pass::IonisedGas);
        // The stars near the camera one by one, and the visited system.
        galaxy_.drawResolvedStars();
        galaxy_.drawSystem(scene_target, scene_width, scene_height);
        break;
    }
    }

    // Running average: new = frame / (n + 1) + old * n / (n + 1).
    float const weight = 1.0f / static_cast<float>(accumulated_frames_ + 1);
    float const blend_factors[4]{weight, weight, weight, weight};
    Diligent::ITextureView* const accumulated_target =
        accumulated_->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET);
    context_->SetBlendFactors(blend_factors);
    drawFullscreen(context_, {&accumulated_target, 1}, scene_width, scene_height,
                   accumulate_pipeline_, accumulate_resources_);
    scene_timer_.end(context_);
    accumulated_frames_ = std::min(accumulated_frames_ + 1, limit - 1);
    metrics.accumulated_frames = accumulated_frames_ + 1;

    bool const post_sample = post_timer_.begin(context_, frame_index_);
    HdrDisplaySettings display;
    display.base_exposure = kBaseExposure[static_cast<std::size_t>(settings.object)];
    display.exposure_ev = settings.exposure_ev;
    display.glare = settings.glare;
    display.tone_map = settings.tone_map;
    display.dither = settings.dither;
    bool const resolved =
        post_.resolve(accumulated_, output, output_width, output_height, display, error);
    post_timer_.end(context_);
    if (!resolved) {
        return false;
    }

    metrics.gpu_sample_new = scene_sample || post_sample;
    metrics.gpu_scene_ms = scene_timer_.latestMs();
    metrics.gpu_post_ms = post_timer_.latestMs();
    ++frame_index_;
    return true;
}

} // namespace helios::examples

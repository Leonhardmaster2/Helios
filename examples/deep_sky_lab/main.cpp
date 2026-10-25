// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Deep-sky lab (Diligent/Vulkan): an emission nebula and a supernova remnant
// (gas-dynamics simulations on the GPU), a disk galaxy (an N-body simulation)
// and a magnetised neutron star.  The black hole of the black-hole lab and
// the planet of the planet LOD lab are in the same object menu.
//
// Routes:
//   - interactive   an SDL window with an orbit camera and ImGui controls;
//   - --capture     a windowless offscreen render written as a PNG;
//   - --benchmark   a windowless timing run;
//   - --validate-gas  the gas-dynamics solver against exact solutions.
// The windowless routes create no window or swap chain, so they never appear
// on, or take input focus from, the desktop.  The interactive route uses no
// mouse grab, relative mouse mode, global input hook, or screen capture.

#include "deep_sky_renderer.hpp"
#include "galaxy_explorer.hpp"
#include "object_variants.hpp"
#include "gas_validation.hpp"
#include "diligent_headless.hpp"
#include "diligent_sdl_app.hpp"
#include "rolling_stats.hpp"

#include "helios/math/vec.hpp"
#include "helios/render/png_writer.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#ifndef HELIOS_GIT_HASH
#define HELIOS_GIT_HASH "unknown"
#endif
#ifndef HELIOS_DILIGENT_COMMIT
#define HELIOS_DILIGENT_COMMIT "unknown"
#endif
#ifndef HELIOS_BUILD_TYPE
#define HELIOS_BUILD_TYPE "unknown"
#endif

namespace helios::examples {
namespace {

using Clock = std::chrono::steady_clock;
using Json = nlohmann::ordered_json;

constexpr double kRadiansPerDegree = std::numbers::pi / 180.0;
// Where the galaxy's run stops: for a capture, and when left to play.
constexpr double kDefaultGalaxyMyr = 600.0;
constexpr double kDefaultGalaxyRunToMyr = 13000.0;

enum class Mode : std::uint8_t { Interactive, Capture, Benchmark, ValidateGas };

struct OrbitCamera {
    double distance = 26.0;
    double elevation_deg = 14.0;
    double azimuth_deg = -90.0;
    double vertical_fov_deg = 40.0;
};

struct ObjectInfo {
    char const* key;   // command-line name
    char const* label; // UI name
    char const* unit;  // length unit of the camera distance
    OrbitCamera camera;
    double minimum_distance;
    double maximum_distance;
    // Scene time advanced per second of wall time in the interactive route.
    double time_rate;
    int steps;
};

constexpr std::array<ObjectInfo, kDeepSkyObjectCount> kObjects{{
    {"nebula", "Emission nebula (gas dynamics)", "pc", {19.0, 12.0, -64.0, 40.0}, 0.5, 200.0, 0.0, 160},
    {"neutron-star", "Neutron star", "M", {95.0, 16.0, -90.0, 36.0}, 8.0, 20000.0, 26.0, 96},
    {"galaxy", "Disk galaxy (N-body)", "kpc", {46.0, 38.0, -70.0, 40.0}, 1.0e-15, 600.0, 0.0, 80},
    {"remnant", "Supernova remnant (gas dynamics)", "pc", {8.6, 18.0, -90.0, 40.0}, 0.3, 80.0, 0.0, 160},
    {"black-hole", "Black hole (Schwarzschild)", "M", {75.0, 6.0, -90.0, 38.0}, 2.2, 2000.0, 6.0, 96},
    // The planet's camera is altitude above ground, latitude and longitude.
    {"planet", "Planet (terrain, atmosphere, rings, moons)", "m", {13'629'000.0, 18.8, -20.0, 60.0}, 2.0, 1.0e9, 600.0, 96},
    {"pulsar", "Pulsar (rotation-powered)", "M", {95.0, 45.0, -90.0, 36.0}, 8.0, 20000.0, 26.0, 96},
}};

// Global quality presets.  One choice drives every object's cost knobs: the
// gas grid and ray-march samples, the galaxy's particle counts, the black
// hole's integration, the neutron star's step size, the planet's level of
// detail, and how many still frames are averaged.
constexpr std::array<char const*, 3> kQualityPresets{"Performance", "Balanced", "Ultra"};
struct QualityValues {
    std::uint32_t gas_cells;       // per edge of the nebula's and the remnant's grid
    float step_factor;             // of each object's default ray-march samples
    float neutron_star_step_scale; // of the neutron star's geodesic steps (smaller: finer)
    std::uint32_t galaxy_disk_stars;
    std::uint32_t galaxy_gas_clouds;
    std::uint32_t galaxy_bulge_stars;
    float planet_lod_error_px; // screen-space error at which a planet tile splits
    int accumulation;          // still frames averaged
    int planet_atmosphere_samples; // of the planet's sky along a ray
    int planet_cloud_samples;      // through its cloud layer
};
constexpr std::array<QualityValues, 3> kQualityValues{{
    {192, 0.7f, 1.5f, 600'000, 400'000, 80'000, 4.0f, 32, 12, 16},
    {256, 1.0f, 1.0f, 1'200'000, 800'000, 160'000, 3.0f, 64, 16, 24},
    {320, 1.3f, 0.6f, 1'500'000, 1'000'000, 200'000, 1.5f, 128, 24, 40},
}};

struct LabState {
    DeepSkySettings scene{};
    std::array<OrbitCamera, kDeepSkyObjectCount> orbits{};
    float render_scale = 1.0f;
    bool v_sync = true;
    bool animate = true;
    float auto_orbit_deg_per_second = 0.0f;
    // Initial conditions edited in the UI; applied by restarting the run.
    NebulaRecipe pending_nebula{};
    RemnantRecipe pending_remnant{};
    // Where the gas runs stop when left to play, and how far each has got.
    double nebula_run_to_myr = 1.3;
    double remnant_run_to_years = 560.0;
    std::array<GasStatus, kDeepSkyObjectCount> gas_status{};
    // Picking, flights and the clock of the galaxy (its speed at the galaxy
    // view is the simulated time gained per second of wall time).
    GalaxyExplorer galaxy_explorer{};
    double galaxy_run_to_myr = kDefaultGalaxyRunToMyr;
    GalaxyRecipe pending_galaxy{};
    int quality_preset = 1;
    // The neutron star's state, and how long one turn takes on screen; the
    // same for the pulsar entry, which is the same model with its own state.
    int neutron_star_variant = static_cast<int>(NeutronStarVariant::YoungPulsar);
    double neutron_star_period_s = 8.0;
    int pulsar_variant = static_cast<int>(NeutronStarVariant::YoungPulsar);
    double pulsar_period_s = 8.0;
    // The chosen variant of the nebula, remnant, galaxy and black hole
    // (object_variants.hpp), by object.
    std::array<int, kDeepSkyObjectCount> variant{};
    // The planet: where the camera looks relative to the horizon, and the
    // scripted descent from deep space to the ground.
    double planet_look_down_deg = 90.0;
    PlanetDescentRoute planet_route{};
};

struct Options {
    Mode mode = Mode::Interactive;
    LabState state{};
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    int supersample = 1;
    int accumulate = 48;
    int frames = 180;
    // --capture of a sequence: frames along an orbit and a time advance.
    int sequence_frames = 1;
    double sequence_orbit_deg = 0.0;
    double sequence_time = 0.0;
    double sequence_turns = 0.0;
    int smoke_frames = 0;
    bool smoke_single = false; // --object was given: the smoke run stays on it
    std::uint32_t threads = std::max(1U, std::thread::hardware_concurrency());
    bool low_priority = false;
    std::optional<bool> validation_layers{};
    // Time to simulate to, in the selected object's unit.  The galaxy
    // defaults to 0 interactively (watch it evolve) and to kDefaultGalaxyMyr
    // for the windowless routes.
    std::optional<double> sim_time{};
    std::uint32_t volume = 128; // cells per edge of --validate-gas
    std::string capture_path{};
    // --galaxy-focus sunlike|ringed|lava|giant|whitedwarf: a star of that
    // kind, found once the run exists.
    std::optional<StarSearch> galaxy_search{};
    bool galaxy_has_distance = false;
    std::string profile_path{};
    std::string report_path{};
    bool show_help = false;
};

[[nodiscard]] ObjectInfo const& infoFor(DeepSkyObject object) {
    return kObjects[static_cast<std::size_t>(object)];
}

void resetState(LabState& state) {
    for (std::size_t index = 0; index < kDeepSkyObjectCount; ++index) {
        state.orbits[index] = kObjects[index].camera;
    }
    state.pending_nebula = state.scene.nebula.recipe;
    state.pending_remnant = state.scene.remnant.recipe;
    state.pending_galaxy = state.scene.galaxy.recipe;
    state.planet_route.playing = false;
}

// Integration quality of the black hole.  Samples per pixel are spent
// adaptively, so the presets differ mostly in how finely edges are resolved.
void applyBlackHolePreset(BlackHoleSettings& scene, int preset) {
    constexpr int kValues[3][4]{{24, 2, 1, 4}, {32, 3, 4, 8}, {48, 4, 8, 12}};
    preset = std::clamp(preset, 0, 2);
    scene.steps_per_half_turn = kValues[preset][0];
    scene.max_image_orders = kValues[preset][1];
    scene.samples_per_pixel = kValues[preset][2];
    scene.critical_samples = kValues[preset][3];
}

[[nodiscard]] int objectSteps(DeepSkyObject object, int preset) {
    double const steps = infoFor(object).steps * kQualityValues[std::clamp(preset, 0, 2)].step_factor;
    return std::clamp(static_cast<int>(std::lround(steps)), 16, 384);
}

// Puts every object on a quality preset.  The gas runs restart at their new
// grid when next shown.  The galaxy's particle counts go to its pending
// initial conditions and take effect on its next restart, or at once with
// `restart_galaxy` (a restart throws away the evolution so far).
void applyQualityPreset(LabState& state, int preset, bool restart_galaxy) {
    state.quality_preset = std::clamp(preset, 0, 2);
    QualityValues const& quality = kQualityValues[state.quality_preset];
    DeepSkySettings& scene = state.scene;
    applyBlackHolePreset(scene.black_hole, state.quality_preset);
    scene.nebula.recipe.resolution = quality.gas_cells;
    scene.remnant.recipe.resolution = quality.gas_cells;
    state.pending_nebula.resolution = quality.gas_cells;
    state.pending_remnant.resolution = quality.gas_cells;
    scene.steps = objectSteps(scene.object, state.quality_preset);
    scene.step_scale = quality.neutron_star_step_scale;
    state.pending_galaxy.disk_stars = quality.galaxy_disk_stars;
    state.pending_galaxy.gas_clouds = quality.galaxy_gas_clouds;
    state.pending_galaxy.bulge_stars = quality.galaxy_bulge_stars;
    if (restart_galaxy) {
        scene.galaxy.recipe = state.pending_galaxy;
        ++scene.galaxy.generation;
    }
    scene.planet.lod_error_px = quality.planet_lod_error_px;
    scene.planet.atmosphere_samples = quality.planet_atmosphere_samples;
    scene.planet.cloud_samples = quality.planet_cloud_samples;
    scene.max_accumulation = quality.accumulation;
}

// The neutron-star entry and the pulsar entry of the menu share the model;
// each has its own settings, state and clock.
[[nodiscard]] bool isNeutronStar(DeepSkyObject object) {
    return object == DeepSkyObject::NeutronStar || object == DeepSkyObject::Pulsar;
}

[[nodiscard]] NeutronStarSettings& starSettings(LabState& state, DeepSkyObject object) {
    return object == DeepSkyObject::Pulsar ? state.scene.pulsar : state.scene.neutron_star;
}

[[nodiscard]] int& starVariant(LabState& state, DeepSkyObject object) {
    return object == DeepSkyObject::Pulsar ? state.pulsar_variant : state.neutron_star_variant;
}

[[nodiscard]] double& starPeriod(LabState& state, DeepSkyObject object) {
    return object == DeepSkyObject::Pulsar ? state.pulsar_period_s : state.neutron_star_period_s;
}

// Puts the neutron star (or the pulsar) into one of its states.  The view
// moves to one that shows it when asked (the states differ in scale).
void applyNeutronStarVariant(LabState& state, DeepSkyObject object, int variant, bool move_camera) {
    variant = std::clamp(variant, 0, static_cast<int>(kNeutronStarVariantCount) - 1);
    starVariant(state, object) = variant;
    auto const kind = static_cast<NeutronStarVariant>(variant);
    NeutronStarSettings& star = starSettings(state, object);
    NeutronStarBand const band = star.band;
    star = neutronStarVariant(kind);
    star.band = band;
    if (move_camera) {
        NeutronStarVariantInfo const& info = neutronStarVariantInfo(kind);
        OrbitCamera& orbit = state.orbits[static_cast<std::size_t>(object)];
        orbit.distance = info.distance;
        orbit.elevation_deg = info.elevation_deg;
        orbit.azimuth_deg = info.azimuth_deg;
        orbit.vertical_fov_deg = info.fov_deg;
    }
}

// Variants of the other objects (object_variants.hpp).
[[nodiscard]] std::span<ObjectVariantInfo const> variantsOf(DeepSkyObject object) {
    switch (object) {
    case DeepSkyObject::Nebula:
        return nebulaVariants();
    case DeepSkyObject::SupernovaRemnant:
        return remnantVariants();
    case DeepSkyObject::Galaxy:
        return galaxyVariants();
    case DeepSkyObject::BlackHole:
        return blackHoleVariants();
    case DeepSkyObject::Planet:
        return planetVariants();
    case DeepSkyObject::NeutronStar:
    case DeepSkyObject::Pulsar:
        break;
    }
    return {};
}

// Puts an object into one of its variants: new initial conditions (the run
// restarts) and the time it is taken to, and when asked a view and an
// exposure that show it.
void applyObjectVariant(LabState& state, DeepSkyObject object, int index, bool move_camera,
                        bool set_exposure) {
    auto const variants = variantsOf(object);
    if (variants.empty()) {
        return;
    }
    auto const choice = static_cast<std::size_t>(
        std::clamp(index, 0, static_cast<int>(variants.size()) - 1));
    ObjectVariantInfo const& info = variants[choice];
    state.variant[static_cast<std::size_t>(object)] = static_cast<int>(choice);
    DeepSkySettings& scene = state.scene;
    switch (object) {
    case DeepSkyObject::Nebula:
        scene.nebula.recipe = nebulaVariantRecipe(choice, scene.nebula.recipe);
        state.pending_nebula = scene.nebula.recipe;
        scene.nebula.time_myr = info.run_to;
        state.nebula_run_to_myr = info.run_to;
        state.gas_status[static_cast<std::size_t>(object)] = {};
        break;
    case DeepSkyObject::SupernovaRemnant:
        scene.remnant.recipe = remnantVariantRecipe(choice, scene.remnant.recipe);
        state.pending_remnant = scene.remnant.recipe;
        scene.remnant.time_years = info.run_to;
        state.remnant_run_to_years = info.run_to;
        state.gas_status[static_cast<std::size_t>(object)] = {};
        break;
    case DeepSkyObject::Galaxy:
        // The pending recipe carries the quality preset's particle counts.
        scene.galaxy.recipe = galaxyVariantRecipe(choice, state.pending_galaxy);
        state.pending_galaxy = scene.galaxy.recipe;
        state.galaxy_run_to_myr = info.run_to;
        ++scene.galaxy.generation;
        break;
    case DeepSkyObject::BlackHole:
        applyBlackHoleVariant(choice, scene.black_hole);
        break;
    case DeepSkyObject::Planet:
        scene.planet.system = planetVariantSystem(choice, scene.planet.system.body.seed);
        scene.planet.patches.body = scene.planet.system.body;
        scene.planet.patches.planet_radius_m = scene.planet.system.body.radius_m;
        state.planet_route.playing = false;
        break;
    case DeepSkyObject::NeutronStar:
    case DeepSkyObject::Pulsar:
        break;
    }
    if (move_camera) {
        OrbitCamera& orbit = state.orbits[static_cast<std::size_t>(object)];
        orbit.distance = info.view.distance;
        orbit.elevation_deg = info.view.elevation_deg;
        orbit.azimuth_deg = info.view.azimuth_deg;
        orbit.vertical_fov_deg = info.view.fov_deg;
    }
    if (set_exposure) {
        scene.exposure_ev = info.exposure_ev;
    }
}

// Command-line name of the object's current variant or state.
[[nodiscard]] char const* variantKey(LabState const& state) {
    DeepSkyObject const object = state.scene.object;
    if (isNeutronStar(object)) {
        return neutronStarVariantInfo(static_cast<NeutronStarVariant>(
                                          object == DeepSkyObject::Pulsar ? state.pulsar_variant
                                                                          : state.neutron_star_variant))
            .key;
    }
    auto const variants = variantsOf(object);
    if (variants.empty()) {
        return "";
    }
    return variants[static_cast<std::size_t>(std::clamp(
                        state.variant[static_cast<std::size_t>(object)], 0,
                        static_cast<int>(variants.size()) - 1))]
        .key;
}

void selectObject(LabState& state, DeepSkyObject object) {
    state.scene.object = object;
    state.scene.steps = objectSteps(object, state.quality_preset);
    // The galactic band is a foreground of our own galaxy; behind another
    // galaxy it would only compete with the subject.
    state.scene.band_intensity = object == DeepSkyObject::Galaxy ? 0.0f : 1.0f;
}

[[nodiscard]] DeepSkyCamera makeCamera(OrbitCamera const& orbit) {
    double const elevation = std::clamp(orbit.elevation_deg, -89.5, 89.5) * kRadiansPerDegree;
    double const azimuth = orbit.azimuth_deg * kRadiansPerDegree;
    math::Vec3d const outward{std::cos(elevation) * std::cos(azimuth),
                              std::cos(elevation) * std::sin(azimuth), std::sin(elevation)};
    DeepSkyCamera camera;
    camera.position = outward * orbit.distance;
    camera.forward = outward * -1.0;
    camera.right = math::normalized(math::cross(camera.forward, math::Vec3d{0.0, 0.0, 1.0}));
    camera.up = math::normalized(math::cross(camera.right, camera.forward));
    camera.vertical_fov_rad = std::clamp(orbit.vertical_fov_deg, 1.0, 150.0) * kRadiansPerDegree;
    return camera;
}

// Height of the ground under the planet camera above the sea level (m).
[[nodiscard]] double groundElevation(PlanetStageSettings const& planet) {
    constexpr double kRadians = std::numbers::pi / 180.0;
    double const latitude = planet.camera.latitude_deg * kRadians;
    double const longitude = planet.camera.longitude_deg * kRadians;
    math::Vec3d const radial{std::cos(latitude) * std::cos(longitude), std::cos(latitude) * std::sin(longitude),
                             std::sin(latitude)};
    return planetHeight(planet.system.body, radial, planet.system.body.max_octaves);
}

void applyCamera(LabState& state) {
    OrbitCamera const& orbit = state.orbits[static_cast<std::size_t>(state.scene.object)];
    state.scene.camera = makeCamera(orbit);
    if (state.scene.object == DeepSkyObject::Planet) {
        // Distance is altitude above ground, elevation latitude and azimuth
        // longitude.
        PlanetStageSettings& planet = state.scene.planet;
        planet.camera.altitude_agl_m = std::clamp(orbit.distance, 2.0, 1.0e9);
        planet.camera.latitude_deg = std::clamp(orbit.elevation_deg, -85.0, 85.0);
        planet.camera.longitude_deg = std::remainder(orbit.azimuth_deg, 360.0);
        planet.camera.look_down_deg = std::clamp(state.planet_look_down_deg, 0.0, 90.0);
        planet.vertical_fov_deg = static_cast<float>(orbit.vertical_fov_deg);
    }
}

[[nodiscard]] double milliseconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>{end - begin}.count();
}

[[nodiscard]] Json describe(LabState const& state, std::string const& adapter) {
    DeepSkySettings const& scene = state.scene;
    ObjectInfo const& info = infoFor(scene.object);
    OrbitCamera const& orbit = state.orbits[static_cast<std::size_t>(scene.object)];
    Json object;
    switch (scene.object) {
    case DeepSkyObject::Nebula: {
        NebulaRecipe const& recipe = scene.nebula.recipe;
        object = {{"model", "gas dynamics with photo-ionisation by one star"},
                  {"seed", recipe.seed},
                  {"cells_per_edge", recipe.resolution},
                  {"half_extent_pc", recipe.half_extent_pc},
                  {"grid_stretch", recipe.stretch},
                  {"cloud_density_cm3", recipe.cloud_density},
                  {"ambient_density_cm3", recipe.ambient_density},
                  {"cloud_radius_pc", recipe.cloud_radius_pc},
                  {"turbulent_speed_km_s", recipe.turbulent_speed_km_s},
                  {"ignition_Myr", recipe.ignition_myr},
                  {"log10_ionising_photons_per_s", recipe.ionising_rate_log10},
                  {"cluster_stars", recipe.cluster_stars},
                  {"ionising_star_Msun", recipe.massive_star_solar},
                  {"star_offset_cloud_radii", recipe.star_offset},
                  {"target_time_Myr", scene.nebula.time_myr},
                  {"emission", scene.nebula.emission},
                  {"dust_opacity", scene.nebula.dust_opacity},
                  {"star_brightness", scene.nebula.star_brightness}};
        break;
    }
    case DeepSkyObject::NeutronStar:
    case DeepSkyObject::Pulsar: {
        bool const pulsar = scene.object == DeepSkyObject::Pulsar;
        NeutronStarSettings const& star = pulsar ? scene.pulsar : scene.neutron_star;
        NeutronStarDerived const d = deriveNeutronStar(star);
        object = {{"model", "Schwarzschild geodesics; dipole magnetosphere; anisotropic crust"},
                  {"state", neutronStarVariantInfo(static_cast<NeutronStarVariant>(
                                                       pulsar ? state.pulsar_variant
                                                              : state.neutron_star_variant))
                                .key},
                  {"band", star.band == NeutronStarBand::XRay ? "x-ray" : "visible"},
                  {"radius_M", star.radius_in_masses},
                  {"spin_hz", star.spin_hz},
                  {"magnetic_inclination_deg", star.magnetic_inclination_deg},
                  {"log10_field_G", star.log10_field_gauss},
                  {"polar_temperature_K", star.polar_temperature_k},
                  {"conductivity_ratio", star.conductivity_ratio},
                  {"polar_cap_temperature_K", star.cap_temperature_k},
                  {"outflow", star.outflow},
                  {"outflow_lorentz_factor", star.lorentz_factor},
                  {"wind_sheet", star.wind_sheet},
                  {"wind_lorentz_factor", star.wind_lorentz_factor},
                  {"twist_glow", star.twist_glow},
                  {"twist_reach_radii", star.twist_reach_radii},
                  {"accreting", star.accreting},
                  {"log10_accretion_rate_Msun_yr", star.log10_accretion_rate},
                  {"orbital_period_h", star.orbital_period_h},
                  {"donor_mass_Msun", star.donor_mass_solar},
                  {"irradiation_fraction", star.irradiation_fraction},
                  {"derived",
                   {{"light_cylinder_M", d.light_cylinder},
                    {"corotation_radius_M", d.corotation_radius},
                    {"equatorial_speed_c", d.equatorial_speed},
                    {"surface_redshift", d.surface_redshift},
                    {"cap_half_angle_deg", d.cap_half_angle_deg},
                    {"spin_down_erg_s", d.spin_down_erg_s},
                    {"magnetospheric_radius_M", d.magnetospheric_radius},
                    {"disk_outer_radius_M", d.disk_outer_radius},
                    {"propeller", d.propeller},
                    {"inner_disk_temperature_K", d.inner_disk_temperature_k},
                    {"spot_temperature_K", d.spot_temperature_k},
                    {"accretion_luminosity_erg_s", d.accretion_luminosity_erg_s}}},
                  {"time_M", scene.time}};
        break;
    }
    case DeepSkyObject::Galaxy: {
        GalaxyRecipe const& recipe = scene.galaxy.recipe;
        object = {{"model", "particle-mesh N-body with gas, star formation and feedback"},
                  {"seed", recipe.seed},
                  {"mesh", recipe.grid},
                  {"half_extent_kpc", recipe.half_extent_kpc},
                  {"disk_stars", recipe.disk_stars},
                  {"gas_clouds", recipe.gas_clouds},
                  {"bulge_stars", recipe.bulge_stars},
                  {"disk_mass_1e10_Msun", recipe.disk_mass},
                  {"gas_mass_1e10_Msun", recipe.gas_mass},
                  {"bulge_mass_1e10_Msun", recipe.bulge_mass},
                  {"disk_scale_kpc", recipe.disk_scale_kpc},
                  {"halo_speed_km_s", recipe.halo_speed_km_s},
                  {"toomre_q", recipe.toomre_q},
                  {"softening_kpc", recipe.softening_kpc},
                  {"time_step_Myr", recipe.time_step_myr},
                  {"black_hole_Msun", recipe.black_hole_mass * 1.0e10},
                  {"target_time_Myr", scene.galaxy.time_myr},
                  {"dust", scene.galaxy.dust},
                  {"ionised_gas", scene.galaxy.ionised_gas},
                  {"smoothing", scene.galaxy.smoothing},
                  {"focus",
                   {{"kind", static_cast<int>(scene.galaxy.focus.kind)},
                    {"particle", scene.galaxy.focus.particle},
                    {"rank", scene.galaxy.focus.rank},
                    {"planet", scene.galaxy.focus.planet},
                    {"moon", scene.galaxy.focus.moon}}},
                  {"resolve_stars", scene.galaxy.resolve_stars},
                  {"star_budget", scene.galaxy.star_budget}};
        break;
    }
    case DeepSkyObject::SupernovaRemnant: {
        RemnantRecipe const& recipe = scene.remnant.recipe;
        object = {{"model", "gas dynamics: supernova ejecta swept up by a pulsar wind"},
                  {"seed", recipe.seed},
                  {"cells_per_edge", recipe.resolution},
                  {"half_extent_pc", recipe.half_extent_pc},
                  {"grid_stretch", recipe.stretch},
                  {"explosion_energy_1e51_erg", recipe.explosion_energy_1e51_erg},
                  {"ejecta_mass_Msun", recipe.ejecta_mass_solar},
                  {"ambient_density_cm3", recipe.ambient_density},
                  {"pulsar_power_1e38_erg_s", recipe.pulsar_power_1e38_erg_s},
                  {"spin_down_years", recipe.spin_down_years},
                  {"braking_index", recipe.braking_index},
                  {"start_years", recipe.start_years},
                  {"clumpiness", recipe.clumpiness},
                  {"envelope_index", recipe.envelope_index},
                  {"radiative", recipe.radiative},
                  {"target_time_years", scene.remnant.time_years},
                  {"filaments", scene.remnant.filaments},
                  {"synchrotron", scene.remnant.synchrotron},
                  {"pulsar", scene.remnant.pulsar},
                  {"dust", scene.remnant.dust}};
        break;
    }
    case DeepSkyObject::BlackHole: {
        BlackHoleSettings const& hole = scene.black_hole;
        object = {{"model", "Schwarzschild geodesics, Page-Thorne thin disk"},
                  {"disk", hole.disk_enabled},
                  {"disk_outer_radius_M", hole.disk_outer_radius_in_masses},
                  {"disk_peak_temperature_K", hole.disk_peak_temperature_k},
                  {"disk_opacity", hole.disk_opacity},
                  {"disk_turbulence", hole.disk_turbulence},
                  {"doppler", hole.doppler},
                  {"gravitational_shift", hole.gravitational_shift},
                  {"curvature", hole.curvature},
                  {"steps_per_half_turn", hole.steps_per_half_turn},
                  {"max_image_orders", hole.max_image_orders},
                  {"samples_per_pixel", hole.samples_per_pixel},
                  {"critical_samples", hole.critical_samples},
                  {"adaptive_sampling", hole.adaptive_sampling},
                  {"time_M", scene.time}};
        break;
    }
    case DeepSkyObject::Planet: {
        PlanetStageSettings const& planet = scene.planet;
        object = {{"model", "cube-sphere quadtree LOD, analytic relief"},
                  {"radius_m", planet.patches.planet_radius_m},
                  {"altitude_m", planet.camera.altitude_agl_m},
                  {"latitude_deg", planet.camera.latitude_deg},
                  {"longitude_deg", planet.camera.longitude_deg},
                  {"look_down_deg", planet.camera.look_down_deg},
                  {"ground_elevation_m", groundElevation(planet)},
                  {"time_s", scene.time},
                  {"day_length_h", planet.system.day_length_s / 3600.0},
                  {"sun_latitude_deg", planet.sun_latitude_deg},
                  {"sun_longitude_deg", planet.sun_longitude_deg},
                  {"gas_giant", planet.system.body.gas_giant},
                  {"atmosphere", planet.system.body.atmosphere},
                  {"clouds", planet.system.clouds.enabled && !planet.system.body.gas_giant},
                  {"cloud_cover", planet.system.clouds.cover},
                  {"rings", planet.system.rings.enabled},
                  {"ring_inner_radii", planet.system.rings.inner_m / planet.system.body.radius_m},
                  {"ring_outer_radii", planet.system.rings.outer_m / planet.system.body.radius_m},
                  {"moons", planet.system.moon_count}};
        break;
    }
    }
    return Json{
        {"build",
         {{"helios_commit", HELIOS_GIT_HASH},
          {"build_type", HELIOS_BUILD_TYPE},
          {"diligent_commit", HELIOS_DILIGENT_COMMIT},
          {"backend", "Diligent Vulkan"},
          {"adapter", adapter}}},
        {"object", info.key},
        {"variant", variantKey(state)},
        {"parameters", object},
        {"camera",
         {{"distance", orbit.distance},
          {"unit", info.unit},
          {"elevation_deg", orbit.elevation_deg},
          {"azimuth_deg", orbit.azimuth_deg},
          {"vertical_fov_deg", orbit.vertical_fov_deg}}},
        {"quality",
         {{"preset", kQualityPresets[static_cast<std::size_t>(state.quality_preset)]},
          {"steps", scene.steps},
          {"neutron_star_step_scale", scene.step_scale}}},
        {"display",
         {{"exposure_ev", scene.exposure_ev},
          {"glare", scene.glare},
          {"tone_map", scene.tone_map ? "ACES fit" : "clamp"}}},
    };
}

[[nodiscard]] bool writeReport(std::string const& path, Json const& report) {
    if (path.empty()) {
        return true;
    }
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file << report.dump(2) << '\n';
    file.flush();
    if (!file.good()) {
        std::fprintf(stderr, "DeepSkyLab: failed to write %s\n", path.c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Windowless routes
// ---------------------------------------------------------------------------

struct Headless {
    std::unique_ptr<DiligentHeadless> device;
    DeepSkyRenderer renderer;
    Diligent::RefCntAutoPtr<Diligent::ITexture> target;
};

[[nodiscard]] bool createHeadless(Headless& headless, Options const& options, bool validation,
                                  std::string& error) {
    headless.device = DiligentHeadless::create(validation, error);
    if (headless.device == nullptr) {
        return false;
    }
    if (!headless.renderer.initialize(headless.device->device(), headless.device->context(),
                                      Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB, options.threads,
                                      error)) {
        return false;
    }
    headless.target =
        createColorTarget(headless.device->device(), "Helios deep-sky offscreen target",
                          options.width, options.height, Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB,
                          false);
    if (headless.target == nullptr) {
        error = "failed to create the offscreen target";
        return false;
    }
    return true;
}

void finishFrame(DiligentHeadless& device) {
    device.context()->Flush();
    device.context()->FinishFrame();
    device.device()->ReleaseStaleResources();
}

// Shell averages of a gas run about the centre of its box: what the
// simulation contains, independent of how it is drawn.
[[nodiscard]] bool writeGasProfile(std::string const& path, GasScene& gas, std::string& error) {
    HydroSimulation& simulation = gas.simulation();
    std::vector<float> a;
    std::vector<float> b;
    if (!simulation.readState(a, b, error)) {
        return false;
    }
    std::size_t const n = simulation.resolution();
    HydroSetup const& setup = simulation.setup();
    double const cell = 2.0 * setup.half_extent / static_cast<double>(n);
    // Cell centres of the stretched grid (lib/hydro.slang).
    auto position_of = [&](std::size_t index) {
        double const xi = (static_cast<double>(index) + 0.5) * (2.0 / static_cast<double>(n)) - 1.0;
        return setup.half_extent *
               (setup.stretch > 0.0 ? std::sinh(setup.stretch * xi) / std::sinh(setup.stretch) : xi);
    };
    struct Shell {
        double count = 0.0;
        double density = 0.0;
        double peak = 0.0;
        double speed = 0.0;
        double pressure = 0.0;
        double scalar1 = 0.0;
        double scalar2 = 0.0;
    };
    std::vector<Shell> shells(n);
    for (std::size_t z = 0; z < n; ++z) {
        for (std::size_t y = 0; y < n; ++y) {
            for (std::size_t x = 0; x < n; ++x) {
                math::Vec3d const p{position_of(x), position_of(y), position_of(z)};
                double const radius = math::length(p);
                auto const index = static_cast<std::size_t>(radius / cell);
                if (index >= n) {
                    continue;
                }
                float const* const ua = a.data() + ((z * n + y) * n + x) * 4U;
                float const* const ub = b.data() + ((z * n + y) * n + x) * 4U;
                double const density = ua[0];
                math::Vec3d const momentum{ua[1], ua[2], ua[3]};
                Shell& shell = shells[index];
                shell.count += 1.0;
                shell.density += density;
                shell.peak = std::max(shell.peak, density);
                shell.speed += math::dot(momentum, p) / (density * std::max(radius, 1.0e-9));
                shell.pressure +=
                    (setup.gamma - 1.0) * (ub[0] - 0.5 * math::dot(momentum, momentum) / density);
                shell.scalar1 += ub[1] / density;
                shell.scalar2 += ub[2] / density;
            }
        }
    }
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file << "radius,mean_density,peak_density,mean_radial_velocity,mean_pressure,mean_scalar1,"
            "mean_scalar2\n";
    for (std::size_t index = 0; index < n; ++index) {
        Shell const& shell = shells[index];
        if (shell.count <= 0.0) {
            continue;
        }
        file << (static_cast<double>(index) + 0.5) * cell << ',' << shell.density / shell.count << ','
             << shell.peak << ',' << shell.speed / shell.count << ',' << shell.pressure / shell.count
             << ',' << shell.scalar1 / shell.count << ',' << shell.scalar2 / shell.count << '\n';
    }
    file.flush();
    if (!file.good()) {
        error = "failed to write " + path;
        return false;
    }
    return true;
}

int runCapture(Options const& options) {
    std::string error;
    Headless headless;
    if (!createHeadless(headless, options, options.validation_layers.value_or(true), error)) {
        std::fprintf(stderr, "DeepSkyLab: %s\n", error.c_str());
        return 1;
    }
    LabState state = options.state;
    applyCamera(state);
    state.scene.max_accumulation = std::max(options.accumulate, 1);
    state.scene.wait_for_simulation = true;
    std::uint32_t const scene_width = options.width * static_cast<std::uint32_t>(options.supersample);
    std::uint32_t const scene_height =
        options.height * static_cast<std::uint32_t>(options.supersample);

    // A still view: every frame uses a different ray-march jitter and the
    // renderer averages them.  A sequence repeats this for frames along an
    // orbit of the camera and an advance of the object's time, both split
    // evenly so that a full orbit loops, written as PATH_000.png, ...
    DeepSkyMetrics metrics;
    bool const galaxy_focus = state.scene.object == DeepSkyObject::Galaxy &&
                              (options.galaxy_search || state.scene.galaxy.focus.kind != GalaxyFocus::Kind::Centre);
    if (galaxy_focus) {
        // One frame builds the run's state; then a star of the kind asked for
        // is chosen, and a second frame works out its system for the viewing
        // distance.
        for (int warm = 0; warm < 2; ++warm) {
            if (!headless.renderer.render(state.scene, scene_width, scene_height,
                                          headless.target->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET),
                                          options.width, options.height, metrics, error)) {
                std::fprintf(stderr, "DeepSkyLab: %s\n", error.c_str());
                return 1;
            }
            finishFrame(*headless.device);
            if (warm == 0 && options.galaxy_search) {
                auto const star = headless.renderer.galaxy().findStar(8.0, *options.galaxy_search);
                if (!star) {
                    std::fprintf(stderr, "DeepSkyLab: no star of that kind found near 8 kpc\n");
                    return 1;
                }
                state.scene.galaxy.focus = *star;
                state.scene.galaxy.previous_focus = *star;
                state.scene.galaxy.transition = 1.0f;
            }
        }
        if (!options.galaxy_has_distance) {
            state.orbits[static_cast<std::size_t>(DeepSkyObject::Galaxy)].distance =
                viewingDistance(headless.renderer.galaxy(), state.scene.galaxy.focus);
        }
    }
    Clock::time_point const begin = Clock::now();
    int const sequence = std::max(options.sequence_frames, 1);
    OrbitCamera const start_orbit = state.orbits[static_cast<std::size_t>(state.scene.object)];
    double const start_time = state.scene.time;
    double time_span = options.sequence_time;
    if (options.sequence_turns > 0.0 && isNeutronStar(state.scene.object)) {
        NeutronStarDerived const d = deriveNeutronStar(starSettings(state, state.scene.object));
        if (d.omega > 0.0) {
            time_span = options.sequence_turns * 2.0 * std::numbers::pi / d.omega;
        }
    }
    render::RgbaImage image;
    image.width = options.width;
    image.height = options.height;
    for (int index = 0; index < sequence; ++index) {
        double const fraction = sequence > 1 ? static_cast<double>(index) / sequence : 0.0;
        OrbitCamera& orbit = state.orbits[static_cast<std::size_t>(state.scene.object)];
        orbit = start_orbit;
        orbit.azimuth_deg = start_orbit.azimuth_deg + options.sequence_orbit_deg * fraction;
        state.scene.time = start_time + time_span * fraction;
        applyCamera(state);
        for (int frame = 0; frame < state.scene.max_accumulation; ++frame) {
            if (!headless.renderer.render(state.scene, scene_width, scene_height,
                                          headless.target->GetDefaultView(
                                              Diligent::TEXTURE_VIEW_RENDER_TARGET),
                                          options.width, options.height, metrics, error)) {
                std::fprintf(stderr, "DeepSkyLab: %s\n", error.c_str());
                return 1;
            }
            finishFrame(*headless.device);
        }
        if (!readbackTexture(headless.device->device(), headless.device->context(), headless.target, 4,
                             image.pixels, error)) {
            std::fprintf(stderr, "DeepSkyLab: %s\n", error.c_str());
            return 1;
        }
        if (sequence > 1) {
            std::string path = options.capture_path;
            char suffix[8];
            std::snprintf(suffix, sizeof suffix, "_%03d", index);
            std::size_t const dot = path.rfind('.');
            path.insert(dot == std::string::npos ? path.size() : dot, suffix);
            auto frame_written = render::writePngRgba(path, image);
            if (frame_written.failed()) {
                std::fprintf(stderr, "DeepSkyLab: %s\n", frame_written.error().to_string().c_str());
                return 1;
            }
        }
    }
    double const elapsed_ms = milliseconds(begin, Clock::now());
    if (!options.profile_path.empty() && (state.scene.object == DeepSkyObject::Nebula ||
                                          state.scene.object == DeepSkyObject::SupernovaRemnant) &&
        !writeGasProfile(options.profile_path, headless.renderer.gas(state.scene.object), error)) {
        std::fprintf(stderr, "DeepSkyLab: %s\n", error.c_str());
        return 1;
    }
    auto written = render::writePngRgba(options.capture_path, image);
    if (written.failed()) {
        std::fprintf(stderr, "DeepSkyLab: %s\n", written.error().to_string().c_str());
        return 1;
    }

    Json report = describe(state, headless.device->adapterName());
    report["image"] = {{"path", options.capture_path},
                       {"width", options.width},
                       {"height", options.height},
                       {"scene_width", scene_width},
                       {"scene_height", scene_height},
                       {"accumulated_frames", state.scene.max_accumulation}};
    if (state.scene.object == DeepSkyObject::Nebula ||
        state.scene.object == DeepSkyObject::SupernovaRemnant) {
        bool const nebula = state.scene.object == DeepSkyObject::Nebula;
        report["simulation"] = {{nebula ? "time_Myr" : "time_years", metrics.gas.time},
                                {"steps", metrics.gas.steps},
                                {"cells_per_edge", metrics.gas.resolution},
                                {"last_time_step", metrics.gas.time_step},
                                {"gas_mass_Msun", metrics.gas.mass},
                                {"setup_seconds", metrics.gas.setup_seconds}};
    }
    if (state.scene.object == DeepSkyObject::Galaxy) {
        report["simulation"] = {{"time_Myr", metrics.galaxy.display_time_myr},
                                {"steps", metrics.galaxy.steps},
                                {"particles", metrics.galaxy.particles},
                                {"stars_formed", metrics.galaxy.stars_formed},
                                {"star_formation_rate_Msun_per_yr", metrics.galaxy.star_formation_rate},
                                {"last_step_ms", metrics.galaxy.step_ms},
                                {"initial_conditions_seconds", metrics.galaxy.initialise_seconds},
                                {"single_stars_drawn", metrics.galaxy.resolved_stars},
                                {"particles_resolved", metrics.galaxy.near_particles},
                                {"pre_exposure", metrics.galaxy.pre_exposure}};
        if (StarSystem const* system = headless.renderer.galaxy().system()) {
            StarState const& star = system->star;
            Json planets = Json::array();
            for (std::uint32_t p = 0; p < system->planet_count; ++p) {
                Planet const& planet = system->planets[p];
                planets.push_back({{"kind", planetKindName(planet.kind)},
                                   {"orbit_AU", planet.orbit_au},
                                   {"mass_Earth", planet.mass_earth},
                                   {"radius_Earth", planet.radius_earth},
                                   {"equilibrium_K", planet.equilibrium_k},
                                   {"period_days", planet.period_days},
                                   {"moons", planet.moon_count},
                                   {"rings", planet.rings}});
            }
            report["system"] = {{"name", system->name},
                                {"class", spectralClass(star)},
                                {"mass_Msun", star.mass},
                                {"luminosity_Lsun", star.luminosity},
                                {"radius_Rsun", star.radius},
                                {"temperature_K", star.temperature_k},
                                {"age_Myr", star.age_myr},
                                {"metallicity", system->metallicity},
                                {"planets", planets}};
        }
    }
    report["elapsed_ms_including_simulation"] = elapsed_ms;
    report["validation_errors"] = headless.device->errorCount();
    if (!writeReport(options.report_path, report)) {
        return 1;
    }
    std::printf("DeepSkyLab: wrote %s (%s, %ux%u, %d frame(s) averaged), %u validation error(s)\n",
                options.capture_path.c_str(), infoFor(state.scene.object).key, options.width,
                options.height, state.scene.max_accumulation, headless.device->errorCount());
    return headless.device->errorCount() == 0 ? 0 : 2;
}

int runBenchmark(Options const& options) {
    std::string error;
    Headless headless;
    bool const validation = options.validation_layers.value_or(false);
    if (!createHeadless(headless, options, validation, error)) {
        std::fprintf(stderr, "DeepSkyLab: %s\n", error.c_str());
        return 1;
    }
    Diligent::ITextureView* const target =
        headless.target->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET);
    LabState state = options.state;
    // Every frame is a new view, as when orbiting: no accumulation.
    state.scene.max_accumulation = 1;
    // A simulated object is timed at its target state, not while it evolves.
    state.scene.wait_for_simulation = true;
    std::uint32_t const scene_width = options.width * static_cast<std::uint32_t>(options.supersample);
    std::uint32_t const scene_height =
        options.height * static_cast<std::uint32_t>(options.supersample);
    OrbitCamera& orbit = state.orbits[static_cast<std::size_t>(state.scene.object)];
    RollingStats scene_ms{4096};
    RollingStats post_ms{4096};

    auto renderFrames = [&](int count, bool record) {
        for (int frame = 0; frame < count; ++frame) {
            orbit.azimuth_deg += 0.25;
            state.scene.time += infoFor(state.scene.object).time_rate / 120.0;
            applyCamera(state);
            DeepSkyMetrics metrics;
            if (!headless.renderer.render(state.scene, scene_width, scene_height, target,
                                          options.width, options.height, metrics, error)) {
                return false;
            }
            finishFrame(*headless.device);
            if (record && metrics.gpu_sample_new) {
                scene_ms.push(metrics.gpu_scene_ms);
                post_ms.push(metrics.gpu_post_ms);
            }
        }
        return true;
    };
    if (!renderFrames(24, false)) {
        std::fprintf(stderr, "DeepSkyLab: %s\n", error.c_str());
        return 1;
    }
    headless.device->context()->WaitForIdle();
    Clock::time_point const begin = Clock::now();
    if (!renderFrames(options.frames, true)) {
        std::fprintf(stderr, "DeepSkyLab: %s\n", error.c_str());
        return 1;
    }
    headless.device->context()->WaitForIdle();
    double const frame_ms = milliseconds(begin, Clock::now()) / static_cast<double>(options.frames);

    Json report = describe(state, headless.device->adapterName());
    report["method"] = "windowless; N frames of a moving camera submitted without presentation "
                       "or pacing, then WaitForIdle; frame time is wall time divided by N";
    report["output_width"] = options.width;
    report["output_height"] = options.height;
    report["scene_width"] = scene_width;
    report["scene_height"] = scene_height;
    report["frames"] = options.frames;
    report["frame_ms"] = frame_ms;
    report["frames_per_second"] = frame_ms > 0.0 ? 1000.0 / frame_ms : 0.0;
    report["gpu_scene_pass_ms_p50"] = scene_ms.percentile(50.0);
    report["gpu_scene_pass_ms_p95"] = scene_ms.percentile(95.0);
    report["gpu_post_passes_ms_p50"] = post_ms.percentile(50.0);
    report["validation_errors"] = headless.device->errorCount();
    if (!writeReport(options.report_path, report)) {
        return 1;
    }
    std::printf("DeepSkyLab benchmark: %s %ux%u, %d steps: frame %.3f ms = %.1f fps "
                "(GPU scene p50 %.3f ms, post p50 %.3f ms)\n",
                infoFor(state.scene.object).key, options.width, options.height, state.scene.steps,
                frame_ms, frame_ms > 0.0 ? 1000.0 / frame_ms : 0.0, scene_ms.percentile(50.0),
                post_ms.percentile(50.0));
    return headless.device->errorCount() == 0 ? 0 : 2;
}

// ---------------------------------------------------------------------------
// Interactive route
// ---------------------------------------------------------------------------

[[nodiscard]] bool sliderDouble(char const* label, double& value, double minimum, double maximum,
                                char const* format,
                                ImGuiSliderFlags flags = ImGuiSliderFlags_None) {
    return ImGui::SliderScalar(label, ImGuiDataType_Double, &value, &minimum, &maximum, format,
                               flags);
}

// The variant combo at the top of an object's panel.
void drawVariantCombo(LabState& state, DeepSkyObject object) {
    auto const variants = variantsOf(object);
    if (variants.empty()) {
        return;
    }
    char const* labels[8];
    int const count = static_cast<int>(std::min<std::size_t>(variants.size(), 8));
    for (int index = 0; index < count; ++index) {
        labels[index] = variants[static_cast<std::size_t>(index)].label;
    }
    int choice = std::clamp(state.variant[static_cast<std::size_t>(object)], 0, count - 1);
    if (ImGui::Combo("Preset", &choice, labels, count)) {
        applyObjectVariant(state, object, choice, true, true);
    }
    ImGui::TextWrapped("%s", variants[static_cast<std::size_t>(choice)].description);
}

// Controls of the neutron star and of the pulsar (the same model).
void drawNeutronStarPanel(LabState& state, DeepSkyObject object) {
    NeutronStarSettings& star = starSettings(state, object);
    int& variant = starVariant(state, object);
    double& period = starPeriod(state, object);
    bool const pulsar = object == DeepSkyObject::Pulsar;
    if (pulsar) {
        // Only the rotation-powered states.
        char const* labels[kPulsarVariants.size()];
        int choice = 0;
        for (std::size_t index = 0; index < kPulsarVariants.size(); ++index) {
            labels[index] = neutronStarVariantInfo(kPulsarVariants[index]).label;
            if (static_cast<int>(kPulsarVariants[index]) == variant) {
                choice = static_cast<int>(index);
            }
        }
        if (ImGui::Combo("State", &choice, labels, static_cast<int>(kPulsarVariants.size()))) {
            applyNeutronStarVariant(
                state, object, static_cast<int>(kPulsarVariants[static_cast<std::size_t>(choice)]),
                true);
        }
    } else {
        char const* variants[kNeutronStarVariantCount];
        for (std::size_t index = 0; index < kNeutronStarVariantCount; ++index) {
            variants[index] = neutronStarVariantInfo(static_cast<NeutronStarVariant>(index)).label;
        }
        int choice = variant;
        if (ImGui::Combo("State", &choice, variants, static_cast<int>(kNeutronStarVariantCount))) {
            applyNeutronStarVariant(state, object, choice, true);
        }
    }
    int band = static_cast<int>(star.band);
    char const* bands[] = {"Visible light", "X-rays (0.2-10 keV shown as colour)"};
    if (ImGui::Combo("Band", &band, bands, 2)) {
        star.band = static_cast<NeutronStarBand>(band);
    }
    (void)sliderDouble("Seconds per turn on screen", period, 1.0, 60.0, "%.1f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Spin (Hz)", &star.spin_hz, 0.05f, 800.0f, "%.2f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Magnetic inclination (deg)", &star.magnetic_inclination_deg, 0.0f, 90.0f,
                       "%.1f");
    ImGui::SliderFloat("log10 dipole field (G)", &star.log10_field_gauss, 7.0f, 16.0f, "%.1f");
    ImGui::SliderFloat("Radius (M)", &star.radius_in_masses, 3.5f, 9.0f, "%.2f");
    ImGui::SeparatorText("Surface");
    ImGui::SliderFloat("Polar temperature (K)", &star.polar_temperature_k, 3.0e4f, 2.0e7f, "%.0f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Conductivity across / along the field", &star.conductivity_ratio, 0.001f,
                       1.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
    bool caps = star.cap_temperature_k > 0.0f;
    if (ImGui::Checkbox("Heated polar caps", &caps)) {
        star.cap_temperature_k = caps ? std::max(2.0f * star.polar_temperature_k, 1.0e6f) : 0.0f;
    }
    if (caps) {
        ImGui::SliderFloat("Cap temperature (K)", &star.cap_temperature_k, 1.0e5f, 3.0e7f, "%.0f",
                           ImGuiSliderFlags_Logarithmic);
    }
    ImGui::SeparatorText("Magnetosphere");
    ImGui::SliderFloat("Open-field outflow", &star.outflow, 0.0f, 4.0f, "%.2f");
    ImGui::SliderFloat("Outflow Lorentz factor", &star.lorentz_factor, 1.0f, 30.0f, "%.1f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Wind current sheet", &star.wind_sheet, 0.0f, 4.0f, "%.2f");
    ImGui::SliderFloat("Wind Lorentz factor", &star.wind_lorentz_factor, 1.0f, 30.0f, "%.1f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Twisted-bundle glow", &star.twist_glow, 0.0f, 4.0f, "%.2f");
    if (star.twist_glow > 0.0f) {
        ImGui::SliderFloat("Bundle reach (stellar radii)", &star.twist_reach_radii, 1.5f, 20.0f,
                           "%.1f", ImGuiSliderFlags_Logarithmic);
    }
    ImGui::SeparatorText("Accretion");
    ImGui::Checkbox("Disk", &star.accreting);
    if (star.accreting) {
        ImGui::SliderFloat("log10 accretion rate (Msun / yr)", &star.log10_accretion_rate, -13.0f,
                           -8.0f, "%.1f");
        ImGui::SliderFloat("Orbital period (h)", &star.orbital_period_h, 0.5f, 200.0f, "%.2f",
                           ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Donor mass (Msun)", &star.donor_mass_solar, 0.01f, 2.0f, "%.3f",
                           ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Irradiation (fraction of L_acc)", &star.irradiation_fraction, 0.0f,
                           0.03f, "%.4f");
        ImGui::SliderFloat("Stream brightness", &star.stream_brightness, 0.0f, 4.0f, "%.2f");
        ImGui::SliderFloat("Stream width (of r_m)", &star.stream_width, 0.05f, 0.6f, "%.2f");
    }
    ImGui::SliderFloat("Surface graticule", &star.grid, 0.0f, 1.0f, "%.2f");

    NeutronStarDerived const d = deriveNeutronStar(star);
    double const radius_km = static_cast<double>(star.radius_in_masses) * 2.0675;
    ImGui::SeparatorText("Derived");
    ImGui::Text("At 1.4 solar masses: %.1f km, surface redshift z = %.3f, equator at %.3f c",
                radius_km, d.surface_redshift, d.equatorial_speed);
    ImGui::Text("Light cylinder %.3g stellar radii (%.3g km); polar caps %.1f deg across",
                d.light_cylinder / star.radius_in_masses, d.light_cylinder * 2.0675,
                2.0 * d.cap_half_angle_deg);
    ImGui::Text("Spin-down power %.2e erg/s (vacuum dipole); shown %.3g times slower",
                d.spin_down_erg_s, period * static_cast<double>(star.spin_hz));
    if (star.accreting) {
        if (d.propeller) {
            ImGui::TextWrapped("Propeller: the disk is held off at %.1f stellar radii, beyond "
                               "corotation (%.1f), so nothing reaches the star.",
                               d.magnetospheric_radius / star.radius_in_masses,
                               d.corotation_radius / star.radius_in_masses);
        } else {
            double const edge_k =
                std::sqrt(std::sqrt(d.irradiation_k4_m2 / (d.disk_outer_radius * d.disk_outer_radius)));
            ImGui::TextWrapped("Disk cut off at %.1f stellar radii (corotation at %.1f); its "
                               "inner edge is at %.2g K, the landing spots at %.2g K, "
                               "%.2e erg/s. Tidal edge at %.3g km (%.2g stellar radii), "
                               "held at %.0f K by irradiation.",
                               d.magnetospheric_radius / star.radius_in_masses,
                               d.corotation_radius / star.radius_in_masses,
                               d.inner_disk_temperature_k, d.spot_temperature_k,
                               d.accretion_luminosity_erg_s, d.disk_outer_radius * 2.0675,
                               d.disk_outer_radius / star.radius_in_masses, edge_k);
        }
    }
    ImGui::TextWrapped("%s", neutronStarVariantInfo(static_cast<NeutronStarVariant>(variant))
                                 .description);
    if (pulsar) {
        ImGui::TextWrapped("The lighthouse: plasma on the open field lines radiates along its "
                           "motion, so each beam is bright only while it points at you; where "
                           "the field lines cross the light cylinder they are wound into the "
                           "striped wind, whose current sheet is the spiral seen from afar. "
                           "The pulse profile follows from the magnetic inclination and the "
                           "viewing angle alone.");
    }
    ImGui::TextWrapped("Light paths are exact Schwarzschild geodesics. Free parameters: the "
                       "brightness of each non-thermal component relative to the surface, "
                       "the Lorentz factors (real ones are higher, at which the beams are "
                       "seen only when they point at you), and the stream width. Not "
                       "modelled: frame dragging, oblateness, light travel time.");
}

void drawControlPanel(DiligentSdlApp& app, LabState& state, DeepSkyMetrics const& metrics,
                      RollingStats const& frame_ms, RollingStats const& scene_ms,
                      RollingStats const& post_ms, GalaxyScene const& galaxy_scene) {
    float const scale = app.uiScale();
    ImGui::SetNextWindowPos(ImVec2{16.0f * scale, 16.0f * scale}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2{460.0f * scale, 760.0f * scale}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Helios v0.0.1 - Deep-Sky Lab")) {
        ImGui::End();
        return;
    }
    DeepSkySettings& scene = state.scene;
    ObjectInfo const& info = infoFor(scene.object);
    OrbitCamera& orbit = state.orbits[static_cast<std::size_t>(scene.object)];

    ImGui::TextWrapped("Adapter: %s", app.adapterName().c_str());
    ImGui::Text("Validation diagnostics: %u warning(s), %u error(s)", app.warningCount(),
                app.errorCount());

    int object = static_cast<int>(scene.object);
    char const* labels[kDeepSkyObjectCount];
    for (std::size_t index = 0; index < kDeepSkyObjectCount; ++index) {
        labels[index] = kObjects[index].label;
    }
    if (ImGui::Combo("Object", &object, labels, static_cast<int>(kDeepSkyObjectCount))) {
        selectObject(state, static_cast<DeepSkyObject>(object));
    }

    if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen)) {
        int preset = state.quality_preset;
        if (ImGui::Combo("Quality preset", &preset, kQualityPresets.data(),
                         static_cast<int>(kQualityPresets.size()))) {
            applyQualityPreset(state, preset, false);
        }
        QualityValues const& quality = kQualityValues[state.quality_preset];
        ImGui::TextWrapped("Gas grid %u^3 (a change restarts the run); galaxy %.1f million "
                           "particles after its next restart; black-hole and planet detail, "
                           "neutron-star step size and frame averaging follow too.",
                           quality.gas_cells,
                           (quality.galaxy_disk_stars + quality.galaxy_gas_clouds +
                            quality.galaxy_bulge_stars) * 1.0e-6);
        double const average = frame_ms.average();
        ImGui::Text("FPS: %.1f", average > 0.0 ? 1000.0 / average : 0.0);
        ImGui::Text("Frame: %.3f ms | p50 %.3f | p95 %.3f | p99 %.3f", frame_ms.latest(),
                    frame_ms.percentile(50.0), frame_ms.percentile(95.0),
                    frame_ms.percentile(99.0));
        if (metrics.gpu_timer_supported && !scene_ms.empty()) {
            ImGui::Text("GPU scene: %.3f ms | p95 %.3f", scene_ms.latest(),
                        scene_ms.percentile(95.0));
            ImGui::Text("GPU glare + composite: %.3f ms", post_ms.latest());
        }
        ImGui::Text("Scene %ux%u, %d frame(s) averaged", metrics.scene_width,
                    metrics.scene_height, metrics.accumulated_frames);
        ImGui::SliderInt("Ray-march steps", &scene.steps, 16, 384);
        ImGui::SliderFloat("Simulation GPU budget (ms)", &scene.simulation_budget_ms, 1.0f, 30.0f,
                           "%.1f");
        if (metrics.gpu_step_ms > 0.0) {
            ImGui::Text("Gas time step: %.2f ms on the GPU, %u this frame", metrics.gpu_step_ms,
                        metrics.simulation_steps);
        }
        ImGui::SliderInt("Average still frames", &scene.max_accumulation, 1, 128);
        ImGui::SliderFloat("Render scale", &state.render_scale, 0.25f, 2.0f, "%.2fx");
        ImGui::Checkbox("V-sync", &state.v_sync);
    }

    if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
        char label[48];
        std::snprintf(label, sizeof(label), "Distance (%s)", info.unit);
        (void)sliderDouble(label, orbit.distance, info.minimum_distance, info.maximum_distance,
                           "%.2f", ImGuiSliderFlags_Logarithmic);
        (void)sliderDouble("Elevation (deg)", orbit.elevation_deg, -89.5, 89.5, "%.2f");
        (void)sliderDouble("Azimuth (deg)", orbit.azimuth_deg, -180.0, 180.0, "%.2f");
        (void)sliderDouble("Vertical FOV (deg)", orbit.vertical_fov_deg, 5.0, 110.0, "%.1f");
        ImGui::SliderFloat("Auto orbit (deg/s)", &state.auto_orbit_deg_per_second, -30.0f, 30.0f,
                           "%.1f");
        ImGui::TextWrapped("Drag with the left mouse button to orbit, wheel to change distance. "
                           "Space pauses, R resets the view, 1-7 select the object, Esc quits. "
                           "In the galaxy a click picks what to visit and Backspace backs out. "
                           "No mouse grab is used.");
    }

    if (ImGui::CollapsingHeader(info.label, ImGuiTreeNodeFlags_DefaultOpen)) {
        switch (scene.object) {
        case DeepSkyObject::Nebula: {
            GasStatus const& status = metrics.gas;
            drawVariantCombo(state, DeepSkyObject::Nebula);
            ImGui::Text("Simulated time: %.3f Myr (step %llu, %.0f yr per step)", status.time,
                        static_cast<unsigned long long>(status.steps), status.time_step * 1.0e6);
            ImGui::Text("%u^3 cells, %.0f solar masses of gas; star %s", status.resolution,
                        status.mass, status.source_on ? "shining" : "not yet formed");
            (void)sliderDouble("Run to (Myr)", state.nebula_run_to_myr, 0.0, 3.0, "%.2f");
            ImGui::SliderFloat("Emission", &scene.nebula.emission, 0.0f, 4.0f, "%.2f");
            ImGui::SliderFloat("Dust (Milky Way = 1)", &scene.nebula.dust_opacity, 0.0f, 4.0f,
                               "%.2f");
            ImGui::SliderFloat("Star brightness", &scene.nebula.star_brightness, 0.0f, 4.0f,
                               "%.2f");
            ImGui::SeparatorText("Initial conditions (restart to apply)");
            NebulaRecipe& pending = state.pending_nebula;
            int seed = static_cast<int>(pending.seed);
            if (ImGui::InputInt("Seed", &seed)) {
                pending.seed = static_cast<std::uint32_t>(std::max(seed, 0));
            }
            (void)sliderDouble("Cloud density (cm^-3)", pending.cloud_density, 50.0, 2000.0, "%.0f",
                               ImGuiSliderFlags_Logarithmic);
            (void)sliderDouble("Surrounding medium (cm^-3)", pending.ambient_density, 0.05, 20.0, "%.2f",
                               ImGuiSliderFlags_Logarithmic);
            (void)sliderDouble("Turbulence (km/s)", pending.turbulent_speed_km_s, 0.5, 8.0, "%.1f");
            (void)sliderDouble("Star switches on (Myr)", pending.ignition_myr, 0.0, 2.5, "%.2f");
            (void)sliderDouble("log10 ionising photons / s", pending.ionising_rate_log10, 45.5,
                               50.5, "%.2f");
            (void)sliderDouble("Ionising star (Msun)", pending.massive_star_solar, 8.0, 80.0, "%.0f",
                               ImGuiSliderFlags_Logarithmic);
            (void)sliderDouble("Star offset (cloud radii)", pending.star_offset, 0.0, 1.0, "%.2f");
            int resolution = static_cast<int>(pending.resolution);
            if (ImGui::SliderInt("Cells per edge", &resolution, 96, 320)) {
                pending.resolution = static_cast<std::uint32_t>(resolution / 16 * 16);
            }
            (void)sliderDouble("Box half-width (pc)", pending.half_extent_pc, 4.0, 40.0, "%.1f");
            (void)sliderDouble("Grid stretch (0 = uniform)", pending.stretch, 0.0, 3.0, "%.2f");
            if (ImGui::Button("Restart from the turbulent cloud")) {
                scene.nebula.recipe = pending;
                scene.nebula.time_myr = 0.0;
                state.gas_status[static_cast<std::size_t>(DeepSkyObject::Nebula)] = {};
            }
            ImGui::TextWrapped("A gas-dynamics run. The cloud starts round and uniform with "
                               "random turbulent motions, which shock it into filaments. Then "
                               "a massive star switches on: gas its ionising photons reach is "
                               "heated to 10^4 K and pushes outwards; clumps that shadow the "
                               "gas behind them remain as pillars. Red is hydrogen, [N II] and "
                               "[S II], teal is [O III] where the radiation is hard, blue is "
                               "starlight scattered by dust.");
            break;
        }
        case DeepSkyObject::NeutronStar:
        case DeepSkyObject::Pulsar:
            drawNeutronStarPanel(state, scene.object);
            break;
        case DeepSkyObject::Galaxy: {
            GalaxySettings& galaxy = scene.galaxy;
            GalaxyStatus const& status = metrics.galaxy;
            drawVariantCombo(state, DeepSkyObject::Galaxy);
            if (!status.ready) {
                ImGui::TextUnformatted("Setting up the initial disk ...");
            }
            ImGui::Text("Shown: %.1f Myr; simulated to %.1f Myr (step %llu, %.1f ms per step)",
                        status.display_time_myr, status.time_myr,
                        static_cast<unsigned long long>(status.steps), status.step_ms);
            ImGui::Text("%u particles; %u of %u gas clouds have formed stars", status.particles,
                        status.stars_formed, status.clouds);
            ImGui::Text("Star formation: %.2f solar masses per year", status.star_formation_rate);
            (void)sliderDouble("Run to (Myr)", state.galaxy_run_to_myr, 0.0, 16000.0, "%.0f");
            ImGui::SliderFloat("Dust (Milky Way = 1)", &galaxy.dust, 0.0f, 3.0f, "%.2f");
            ImGui::SliderFloat("Ionised gas (computed = 1)", &galaxy.ionised_gas, 0.0f, 3.0f,
                               "%.2f");
            ImGui::SliderFloat("Star image width", &galaxy.smoothing, 0.5f, 8.0f, "%.2f");
            ImGui::SeparatorText("Initial conditions (restart to apply)");
            int seed = static_cast<int>(state.pending_galaxy.seed);
            if (ImGui::InputInt("Seed", &seed)) {
                state.pending_galaxy.seed = static_cast<std::uint32_t>(std::max(seed, 0));
            }
            (void)sliderDouble("Toomre Q", state.pending_galaxy.toomre_q, 0.8, 2.5, "%.2f");
            (void)sliderDouble("Gas mass (1e10 Msun)", state.pending_galaxy.gas_mass, 0.05, 2.0,
                               "%.2f");
            (void)sliderDouble("Bulge mass (1e10 Msun)", state.pending_galaxy.bulge_mass, 0.05, 3.0,
                               "%.2f");
            (void)sliderDouble("Halo speed (km/s)", state.pending_galaxy.halo_speed_km_s, 80.0,
                               260.0, "%.0f");
            if (ImGui::Button("Restart from a smooth disk")) {
                galaxy.recipe = state.pending_galaxy;
                ++galaxy.generation;
            }
            ImGui::TextWrapped("Every point of light is a star particle of an N-body run that "
                               "started as a smooth, featureless disk. Arms, bar and dust lanes "
                               "are made by gravity; colour is the age of the stars; pink is gas "
                               "ionised by stars a few Myr old. The dark halo, the bulge's "
                               "gravity and the central black hole are fixed potentials. Each "
                               "particle is the tens of thousands of stars it stands for: near "
                               "the camera they are drawn one by one, and each has its planets.");
            drawExplorerPanel(state.galaxy_explorer, galaxy, galaxy_scene,
                              state.orbits[static_cast<std::size_t>(DeepSkyObject::Galaxy)].distance);
            break;
        }
        case DeepSkyObject::SupernovaRemnant: {
            RemnantSettings& remnant = scene.remnant;
            GasStatus const& status = metrics.gas;
            drawVariantCombo(state, DeepSkyObject::SupernovaRemnant);
            ImGui::Text("Age: %.0f years (step %llu, %.2f yr per step)", status.time,
                        static_cast<unsigned long long>(status.steps), status.time_step);
            ImGui::Text("%u^3 cells, %.2f solar masses in the box", status.resolution, status.mass);
            (void)sliderDouble("Run to (years)", state.remnant_run_to_years, 30.0, 20000.0, "%.0f",
                               ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Filament light", &remnant.filaments, 0.0f, 3.0f, "%.2f");
            ImGui::SliderFloat("Synchrotron light", &remnant.synchrotron, 0.0f, 3.0f, "%.2f");
            ImGui::SliderFloat("Pulsar", &remnant.pulsar, 0.0f, 4.0f, "%.2f");
            ImGui::SliderFloat("Dust (Milky Way = 1)", &remnant.dust, 0.0f, 10.0f, "%.2f");
            ImGui::SeparatorText("Initial conditions (restart to apply)");
            RemnantRecipe& pending = state.pending_remnant;
            int seed = static_cast<int>(pending.seed);
            if (ImGui::InputInt("Seed", &seed)) {
                pending.seed = static_cast<std::uint32_t>(std::max(seed, 0));
            }
            (void)sliderDouble("Explosion energy (1e51 erg)", pending.explosion_energy_1e51_erg,
                               0.03, 1.0, "%.2f", ImGuiSliderFlags_Logarithmic);
            (void)sliderDouble("Ejecta mass (Msun)", pending.ejecta_mass_solar, 1.0, 12.0, "%.1f");
            (void)sliderDouble("Surrounding gas (cm^-3)", pending.ambient_density, 0.1, 20.0, "%.1f",
                               ImGuiSliderFlags_Logarithmic);
            (void)sliderDouble("Pulsar power (1e38 erg/s)", pending.pulsar_power_1e38_erg_s, 0.0,
                               200.0, "%.0f", ImGuiSliderFlags_Logarithmic);
            (void)sliderDouble("Ejecta clumpiness", pending.clumpiness, 0.0, 0.3, "%.2f");
            (void)sliderDouble("Envelope index n", pending.envelope_index, 6.0, 12.0, "%.1f");
            ImGui::Checkbox("Swept-up gas cools (radiative phase)", &pending.radiative);
            (void)sliderDouble("Box half-width (pc)", pending.half_extent_pc, 1.0, 20.0, "%.1f");
            (void)sliderDouble("Grid stretch (0 = uniform)", pending.stretch, 0.0, 3.0, "%.2f");
            int resolution = static_cast<int>(pending.resolution);
            if (ImGui::SliderInt("Cells per edge", &resolution, 96, 320)) {
                pending.resolution = static_cast<std::uint32_t>(resolution / 16 * 16);
            }
            if (ImGui::Button("Restart from the explosion")) {
                remnant.recipe = pending;
                remnant.time_years = pending.start_years;
                state.gas_status[static_cast<std::size_t>(DeepSkyObject::SupernovaRemnant)] = {};
            }
            ImGui::TextWrapped("A gas-dynamics run. Smoothly expanding supernova ejecta plough "
                               "into the gas around the star. The blast wave runs ahead (the thin "
                               "red rim, hydrogen light); behind it the dense ejecta are slowed "
                               "by the light gas they swept up, which is unstable, and their "
                               "boundary breaks into fingers (white: hot plasma). At the centre "
                               "the wind of the pulsar blows a bubble that shines blue-white. No "
                               "magnetic field, so no jet or torus.");
            break;
        }
        case DeepSkyObject::BlackHole: {
            BlackHoleSettings& hole = scene.black_hole;
            drawVariantCombo(state, DeepSkyObject::BlackHole);
            ImGui::Text("Integration (quality preset): %d steps per half turn, %d image orders, "
                        "%d / %d samples",
                        hole.steps_per_half_turn, hole.max_image_orders, hole.samples_per_pixel,
                        hole.critical_samples);
            ImGui::Checkbox("Adaptive supersampling", &hole.adaptive_sampling);
            ImGui::Checkbox("Disk", &hole.disk_enabled);
            ImGui::SameLine();
            ImGui::Checkbox("Limb darkening", &hole.limb_darkening);
            ImGui::SliderFloat("Outer radius (M)", &hole.disk_outer_radius_in_masses, 8.0f, 200.0f,
                               "%.1f", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Peak temperature (K)", &hole.disk_peak_temperature_k, 1000.0f,
                               500000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Opacity", &hole.disk_opacity, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Streaks (painted)", &hole.disk_turbulence, 0.0f, 0.95f, "%.2f");
            ImGui::SeparatorText("Relativistic effects");
            ImGui::Checkbox("Curved space-time (light bending)", &hole.curvature);
            ImGui::Checkbox("Doppler shift and beaming", &hole.doppler);
            ImGui::Checkbox("Gravitational redshift", &hole.gravitational_shift);
            int sky_mode = static_cast<int>(hole.sky_mode);
            char const* sky_modes[] = {"Procedural stars", "Celestial grid", "Black"};
            if (ImGui::Combo("Sky", &sky_mode, sky_modes, 3)) {
                hole.sky_mode = static_cast<BlackHoleSkyMode>(sky_mode);
            }
            ImGui::TextWrapped("Every ray is an exact Schwarzschild null geodesic; the disk is a "
                               "Page-Thorne thin disk, its colour a black body shifted by Doppler "
                               "and gravity. All three effects on is the physical image. The "
                               "streak texture of the disk is the one painted element; set it to "
                               "zero for the bare model. The disk is far cooler than a real one "
                               "so that its colour structure shows.");
            break;
        }
        case DeepSkyObject::Planet: {
            PlanetStageSettings& planet = scene.planet;
            PlanetStageMetrics const& status = metrics.planet;
            ImGui::Text("%u quadtree leaves, %llu triangles, %u draw call(s)", status.leaves,
                        static_cast<unsigned long long>(status.patches.triangles),
                        status.patches.draw_calls);
            ImGui::Text("Largest screen-space error %.2f px; selection %.2f ms", status.max_error_px,
                        status.selection_ms);
            drawVariantCombo(state, DeepSkyObject::Planet);
            bool playing = state.planet_route.playing;
            if (ImGui::Checkbox("Play descent", &playing)) {
                state.planet_route.playing = playing;
            }
            ImGui::SameLine();
            if (ImGui::Button("Restart descent")) {
                state.planet_route.restart(planet.camera);
            }
            ImGui::ProgressBar(static_cast<float>(state.planet_route.progress()), ImVec2{-1.0f, 0.0f},
                               state.planet_route.stageLabel());
            (void)sliderDouble("Look down (deg)", state.planet_look_down_deg, 0.0, 90.0, "%.1f");
            int debug_color = static_cast<int>(planet.patches.debug_color);
            char const* colors[] = {"Natural", "LOD level", "Cube face", "Screen-space error"};
            if (ImGui::Combo("Colour", &debug_color, colors, 4)) {
                planet.patches.debug_color = static_cast<PatchDebugColor>(debug_color);
            }
            ImGui::Checkbox("Wireframe", &planet.patches.wire_overlay);
            ImGui::SameLine();
            ImGui::Checkbox("Horizon culling", &planet.patches.horizon_culling);
            PlanetSystem& system = planet.system;
            PlanetBody& body = system.body;
            double const radius = body.radius_m;
            if (ImGui::CollapsingHeader("Surface")) {
                if (body.gas_giant) {
                    (void)sliderDouble("Bands per hemisphere", body.bands_per_hemisphere, 2.0, 14.0, "%.0f");
                    (void)sliderDouble("Band turbulence", body.band_turbulence, 0.0, 2.0, "%.2f");
                    (void)sliderDouble("Storms", body.storm_density, 0.0, 1.0, "%.2f");
                } else {
                    (void)sliderDouble("Land bias", body.land_bias, -1.0, 1.0, "%.2f");
                    (void)sliderDouble("Mountains (m)", body.mountain_amplitude_m, 0.0, 20000.0, "%.0f");
                    ImGui::Checkbox("Ocean", &body.ocean);
                    ImGui::SameLine();
                    ImGui::Checkbox("Atmosphere", &body.atmosphere);
                    (void)sliderDouble("Sea level (m)", body.sea_level_m, -3000.0, 3000.0, "%.0f");
                    (void)sliderDouble("Craters (per cell)", body.crater_density, 0.0, 1.0, "%.2f");
                    (void)sliderDouble("Equator temperature (K)", body.equator_temperature_k, 150.0,
                                       400.0, "%.0f");
                }
                int seed = static_cast<int>(body.seed);
                if (ImGui::InputInt("Terrain seed", &seed)) {
                    body.seed = static_cast<std::uint32_t>(std::max(seed, 0));
                }
            }
            if (ImGui::CollapsingHeader("Sun and time")) {
                (void)sliderDouble("Sun latitude (deg)", planet.sun_latitude_deg, -90.0, 90.0, "%.1f");
                (void)sliderDouble("Sun longitude at t = 0 (deg)", planet.sun_longitude_deg, -180.0,
                                   180.0, "%.1f");
                double day_h = system.day_length_s / 3600.0;
                if (sliderDouble("Day length (h)", day_h, 1.0, 2000.0, "%.1f",
                                 ImGuiSliderFlags_Logarithmic)) {
                    system.day_length_s = day_h * 3600.0;
                }
                ImGui::Text("Planet time %.2f days; runs at %.0fx when animating",
                            scene.time / std::max(system.day_length_s, 1.0),
                            infoFor(DeepSkyObject::Planet).time_rate);
            }
            if (!body.gas_giant && ImGui::CollapsingHeader("Clouds")) {
                PlanetClouds& clouds = system.clouds;
                ImGui::Checkbox("Cloud layer", &clouds.enabled);
                (void)sliderDouble("Cover", clouds.cover, 0.0, 1.0, "%.2f");
                double base_km = clouds.base_m / 1000.0;
                if (sliderDouble("Base (km)", base_km, 0.2, 30.0, "%.1f", ImGuiSliderFlags_Logarithmic)) {
                    clouds.base_m = base_km * 1000.0;
                    clouds.top_m = std::max(clouds.top_m, clouds.base_m + 200.0);
                }
                double top_km = clouds.top_m / 1000.0;
                if (sliderDouble("Top (km)", top_km, 0.4, 40.0, "%.1f", ImGuiSliderFlags_Logarithmic)) {
                    clouds.top_m = std::max(top_km * 1000.0, clouds.base_m + 200.0);
                }
                double extinction_km = clouds.extinction_per_m * 1000.0;
                if (sliderDouble("Extinction (1/km)", extinction_km, 0.5, 100.0, "%.1f",
                                 ImGuiSliderFlags_Logarithmic)) {
                    clouds.extinction_per_m = extinction_km / 1000.0;
                }
                double wind = clouds.wind_rad_per_s * radius;
                if (sliderDouble("Zonal wind (m/s)", wind, 0.0, 150.0, "%.0f")) {
                    clouds.wind_rad_per_s = wind / std::max(radius, 1.0);
                }
            }
            if (ImGui::CollapsingHeader("Rings")) {
                PlanetRings& rings = system.rings;
                if (ImGui::Checkbox("Rings", &rings.enabled) && rings.enabled &&
                    rings.outer_m <= rings.inner_m) {
                    rings.inner_m = 1.3 * radius;
                    rings.outer_m = 2.1 * radius;
                }
                double inner = rings.inner_m / radius;
                if (sliderDouble("Inner edge (radii)", inner, 1.05, 4.0, "%.2f")) {
                    rings.inner_m = inner * radius;
                    rings.outer_m = std::max(rings.outer_m, rings.inner_m + 0.02 * radius);
                }
                double outer = rings.outer_m / radius;
                if (sliderDouble("Outer edge (radii)", outer, 1.1, 5.0, "%.2f")) {
                    rings.outer_m = std::max(outer * radius, rings.inner_m + 0.02 * radius);
                }
                (void)sliderDouble("Peak optical depth", rings.peak_optical_depth, 0.05, 3.0, "%.2f",
                                   ImGuiSliderFlags_Logarithmic);
                (void)sliderDouble("Particle albedo", rings.albedo, 0.1, 0.95, "%.2f");
                (void)sliderDouble("Ringlet contrast", rings.ringlet_contrast, 0.0, 1.0, "%.2f");
                ImGui::Text("Roche limit for ice (920 kg/m^3): %.2f radii",
                            planetRocheLimit(system, 920.0) / radius);
            }
            if (ImGui::CollapsingHeader("Moons")) {
                int count = static_cast<int>(system.moon_count);
                if (ImGui::SliderInt("Moons", &count, 0, static_cast<int>(kMaxPlanetMoons))) {
                    for (int index = static_cast<int>(system.moon_count); index < count; ++index) {
                        PlanetMoon& moon = system.moons[static_cast<std::size_t>(index)];
                        if (moon.body.radius_m <= 0.0 || moon.orbit_radius_m <= 0.0) {
                            moon.body = planetBodyPreset(PlanetBodyKind::RockyMoon,
                                                         body.seed + 20U + static_cast<std::uint32_t>(index));
                            moon.body.radius_m = 0.15 * radius;
                            moon.orbit_radius_m = (8.0 + 6.0 * index) * radius;
                            moon.mass_ratio = 3300.0 * 0.15 * 0.15 * 0.15 / system.mean_density_kg_m3;
                            moon.phase_rad = 1.3 * index;
                        }
                    }
                    system.moon_count = static_cast<std::uint32_t>(count);
                }
                for (std::uint32_t index = 0; index < system.moon_count; ++index) {
                    ImGui::PushID(static_cast<int>(index));
                    PlanetMoon& moon = system.moons[index];
                    double orbit_radii = moon.orbit_radius_m / radius;
                    if (sliderDouble("Orbit (radii)", orbit_radii, 1.5, 100.0, "%.1f",
                                     ImGuiSliderFlags_Logarithmic)) {
                        moon.orbit_radius_m = orbit_radii * radius;
                    }
                    double moon_km = moon.body.radius_m / 1000.0;
                    if (sliderDouble("Radius (km)", moon_km, 5.0, 3000.0, "%.0f", ImGuiSliderFlags_Logarithmic)) {
                        moon.body.radius_m = moon_km * 1000.0;
                        double const ratio = moon.body.radius_m / radius;
                        moon.mass_ratio = 3000.0 * ratio * ratio * ratio / system.mean_density_kg_m3;
                    }
                    double const motion = moonMeanMotion(system, moon);
                    ImGui::Text("Moon %u: period %.2f days, Hill radius %.0f km", index + 1,
                                motion > 0.0 ? 2.0 * std::numbers::pi / motion / 86400.0 : 0.0,
                                moonHillRadius(system, moon) / 1000.0);
                    ImGui::PopID();
                }
            }
            ImGui::TextWrapped("A planetary system drawn from a billion metres away down to eye "
                               "level: terrain from a physically scaled height field, an "
                               "atmosphere by single scattering with a precomputed transmittance "
                               "table, volumetric clouds, rings shaded as a single-scattering slab "
                               "with their gaps at the moons' resonances, and moons on Kepler orbits "
                               "in their own frames. In the camera panel, distance is altitude, "
                               "elevation latitude and azimuth longitude. Unlike the other objects "
                               "it is not a simulation: time moves the sun, the clouds and the "
                               "moons.");
            break;
        }
        }
    }

    if (ImGui::CollapsingHeader("Sky and display", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Star intensity", &scene.star_intensity, 0.0f, 8.0f, "%.2f",
                           ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Galactic band", &scene.band_intensity, 0.0f, 8.0f, "%.2f",
                           ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Exposure (EV)", &scene.exposure_ev, -8.0f, 8.0f, "%.2f");
        ImGui::SliderFloat("Glare", &scene.glare, 0.0f, 0.5f, "%.3f");
        ImGui::Checkbox("ACES tone map", &scene.tone_map);
        ImGui::SameLine();
        ImGui::Checkbox("Dither", &scene.dither);
        ImGui::Checkbox("Animate", &state.animate);
    }
    ImGui::End();
}

int runInteractive(Options const& options) {
    DiligentSdlCreateInfo create_info;
    create_info.title = "Helios v0.0.1 - Vulkan Deep-Sky Lab";
    // Validation layers roughly double the cost of a simulation frame; the
    // windowless routes keep them on, the window runs without unless asked.
    create_info.enable_validation = options.validation_layers.value_or(false);
    create_info.hidden = options.smoke_frames > 0;
    std::string error;
    auto app = DiligentSdlApp::create(create_info, error);
    if (app == nullptr) {
        std::fprintf(stderr, "Deep-sky lab initialization failed: %s\n", error.c_str());
        return 1;
    }
    DeepSkyRenderer renderer;
    if (!renderer.initialize(app->device(), app->context(),
                             app->swapChain()->GetDesc().ColorBufferFormat, options.threads,
                             error)) {
        std::fprintf(stderr, "Deep-sky renderer initialization failed: %s\n", error.c_str());
        return 1;
    }

    LabState state = options.state;
    std::optional<StarSearch> pending_search = options.galaxy_search;
    RollingStats frame_ms{360};
    RollingStats scene_ms{360};
    RollingStats post_ms{360};
    DeepSkyMetrics metrics;
    std::vector<RollingStats> smoke_ms(kDeepSkyObjectCount, RollingStats{100000});
    std::array<int, kDeepSkyObjectCount> smoke_shown{};
    std::array<std::vector<int>, kDeepSkyObjectCount> smoke_slow{};
    std::vector<RollingStats> smoke_simulation_ms(kDeepSkyObjectCount, RollingStats{100000});
    std::vector<RollingStats> smoke_scene_ms(kDeepSkyObjectCount, RollingStats{100000});

    bool running = true;
    bool orbiting = false;
    // A press and release of the left button without moving is a click.
    float press_x = 0.0f;
    float press_y = 0.0f;
    bool pressed = false;
    int frame_count = 0;
    Clock::time_point previous_frame = Clock::now();
    while (running) {
        Clock::time_point const frame_begin = Clock::now();
        double const dt_seconds = std::clamp(
            std::chrono::duration<double>{frame_begin - previous_frame}.count(), 0.0, 0.1);
        previous_frame = frame_begin;
        std::size_t const object_index = static_cast<std::size_t>(state.scene.object);
        OrbitCamera& orbit = state.orbits[object_index];

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            app->forwardEvent(event);
            switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                running = false;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.repeat || app->wantsKeyboardCapture()) {
                    break;
                }
                if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                    running = false;
                } else if (event.key.scancode == SDL_SCANCODE_SPACE) {
                    state.animate = !state.animate;
                } else if (event.key.scancode == SDL_SCANCODE_R) {
                    orbit = kObjects[object_index].camera;
                    if (state.scene.object == DeepSkyObject::Galaxy) {
                        state.scene.galaxy.focus = GalaxyFocus{};
                        state.scene.galaxy.previous_focus = GalaxyFocus{};
                        state.scene.galaxy.transition = 1.0f;
                        state.galaxy_explorer.flight_seconds = 0.0;
                    }
                } else if (event.key.scancode == SDL_SCANCODE_BACKSPACE &&
                           state.scene.object == DeepSkyObject::Galaxy) {
                    GalaxyFocus const parent = parentFocus(state.scene.galaxy.focus);
                    flyTo(state.galaxy_explorer, state.scene.galaxy, parent, orbit.distance,
                          std::max(viewingDistance(renderer.galaxy(), parent), orbit.distance * 4.0));
                } else if (event.key.scancode >= SDL_SCANCODE_1 &&
                           event.key.scancode <= SDL_SCANCODE_7) {
                    selectObject(state, static_cast<DeepSkyObject>(event.key.scancode -
                                                                   SDL_SCANCODE_1));
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (!app->wantsMouseCapture() && event.button.button == SDL_BUTTON_LEFT) {
                    orbiting = true;
                    pressed = true;
                    press_x = event.button.x;
                    press_y = event.button.y;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.button.button == SDL_BUTTON_LEFT) {
                    orbiting = false;
                    bool const click = pressed && std::abs(event.button.x - press_x) < 4.0f &&
                                       std::abs(event.button.y - press_y) < 4.0f;
                    pressed = false;
                    if (click && state.scene.object == DeepSkyObject::Galaxy) {
                        int window_width = 1;
                        int window_height = 1;
                        SDL_GetWindowSize(app->window(), &window_width, &window_height);
                        auto const picked = renderer.galaxy().pick(
                            event.button.x / static_cast<double>(std::max(window_width, 1)),
                            event.button.y / static_cast<double>(std::max(window_height, 1)));
                        if (picked && !(*picked == state.scene.galaxy.focus)) {
                            flyTo(state.galaxy_explorer, state.scene.galaxy, *picked, orbit.distance,
                                  picked->kind == GalaxyFocus::Kind::Particle
                                      ? std::min(orbit.distance, viewingDistance(renderer.galaxy(), *picked))
                                      : viewingDistance(renderer.galaxy(), *picked),
                                  picked->kind == GalaxyFocus::Kind::Particle ? 2.0 : 3.0);
                        }
                    }
                }
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                orbiting = false;
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (orbiting) {
                    double const degrees_per_pixel =
                        orbit.vertical_fov_deg / std::max(1.0, static_cast<double>(app->height()));
                    // Above a planet the drag moves the ground under the
                    // camera, by less the lower it flies.
                    double const reach =
                        state.scene.object == DeepSkyObject::Planet
                            ? std::min(orbit.distance / state.scene.planet.patches.planet_radius_m, 3.0)
                            : 3.0;
                    orbit.azimuth_deg -= event.motion.xrel * degrees_per_pixel * reach;
                    orbit.elevation_deg += event.motion.yrel * degrees_per_pixel * reach;
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                if (!app->wantsMouseCapture()) {
                    // The galaxy spans kiloparsecs to kilometres.
                    double const step = state.scene.object == DeepSkyObject::Galaxy ? 0.25 : 0.12;
                    orbit.distance *= std::exp(-step * event.wheel.y);
                    if (state.scene.object == DeepSkyObject::Galaxy && state.galaxy_explorer.flight_seconds > 0.0) {
                        state.galaxy_explorer.to_distance *= std::exp(-step * event.wheel.y);
                    }
                }
                break;
            default:
                break;
            }
        }
        if (!running) {
            break;
        }
        if (!app->synchronizeDrawableSize()) {
            SDL_Delay(16);
            continue;
        }

        ObjectInfo const& info = infoFor(state.scene.object);
        OrbitCamera& active = state.orbits[static_cast<std::size_t>(state.scene.object)];
        active.azimuth_deg = std::remainder(
            active.azimuth_deg + static_cast<double>(state.auto_orbit_deg_per_second) * dt_seconds,
            360.0);
        active.elevation_deg = std::clamp(active.elevation_deg, -89.5, 89.5);
        active.distance = std::clamp(active.distance, info.minimum_distance, info.maximum_distance);
        // A gas run plays towards its stop time, or holds where it is.
        std::size_t const active_index = static_cast<std::size_t>(state.scene.object);
        if (state.scene.object == DeepSkyObject::Nebula) {
            double const reached = state.gas_status[active_index].time;
            state.scene.nebula.time_myr =
                state.animate ? std::max(state.nebula_run_to_myr, reached) : reached;
        } else if (state.scene.object == DeepSkyObject::SupernovaRemnant) {
            double const reached =
                std::max(state.gas_status[active_index].time, state.scene.remnant.recipe.start_years);
            state.scene.remnant.time_years =
                state.animate ? std::max(state.remnant_run_to_years, reached) : reached;
        }
        // The planet's scripted descent drives its camera while it plays.
        if (state.scene.object == DeepSkyObject::Planet && state.planet_route.playing) {
            PlanetCamera camera = state.scene.planet.camera;
            state.planet_route.advance(dt_seconds, camera);
            active.distance = camera.altitude_agl_m;
            active.elevation_deg = camera.latitude_deg;
            active.azimuth_deg = camera.longitude_deg;
            state.planet_look_down_deg = camera.look_down_deg;
        }
        // The galaxy plays towards its stop time at a speed that follows the
        // scale in view, or holds; a flight moves its camera.
        state.scene.galaxy.time_myr = state.galaxy_run_to_myr;
        if (state.scene.object == DeepSkyObject::Galaxy) {
            updateExplorer(state.galaxy_explorer, state.scene.galaxy, renderer.galaxy(), active.distance, dt_seconds,
                           state.animate);
        } else {
            state.scene.galaxy.speed_myr_per_second = 0.0;
        }
        if (state.animate) {
            if (isNeutronStar(state.scene.object)) {
                // One turn of the star takes the chosen number of seconds.
                NeutronStarDerived const d =
                    deriveNeutronStar(starSettings(state, state.scene.object));
                if (d.omega > 0.0) {
                    state.scene.time +=
                        dt_seconds *
                        (2.0 * std::numbers::pi / std::max(starPeriod(state, state.scene.object), 0.1)) /
                        d.omega;
                }
            } else {
                state.scene.time += info.time_rate * dt_seconds;
            }
        }
        applyCamera(state);

        app->beginImGuiFrame();
        float const scale = std::clamp(state.render_scale, 0.25f, 2.0f);
        std::uint32_t const scene_width = std::max(
            1U, static_cast<std::uint32_t>(std::lround(static_cast<float>(app->width()) * scale)));
        std::uint32_t const scene_height = std::max(
            1U, static_cast<std::uint32_t>(std::lround(static_cast<float>(app->height()) * scale)));
        Diligent::ITextureView* back_buffer = app->swapChain()->GetCurrentBackBufferRTV();
        if (!renderer.render(state.scene, scene_width, scene_height, back_buffer, app->width(),
                             app->height(), metrics, error)) {
            std::fprintf(stderr, "Deep-sky render failed: %s\n", error.c_str());
            return 1;
        }
        if (metrics.gpu_sample_new) {
            scene_ms.push(metrics.gpu_scene_ms);
            post_ms.push(metrics.gpu_post_ms);
        }
        state.gas_status[static_cast<std::size_t>(state.scene.object)] = metrics.gas;
        // --galaxy-focus with a kind of star: fly there once the run exists.
        if (pending_search && state.scene.object == DeepSkyObject::Galaxy && metrics.galaxy.ready) {
            if (auto const star = renderer.galaxy().findStar(8.0, *pending_search)) {
                OrbitCamera& galaxy_orbit = state.orbits[static_cast<std::size_t>(DeepSkyObject::Galaxy)];
                flyTo(state.galaxy_explorer, state.scene.galaxy, *star, galaxy_orbit.distance, 5.0 / 2.06264806e8, 4.0);
            }
            pending_search.reset();
        }

        // The ImGui pipeline was created for the swap chain's colour + depth pair.
        app->context()->SetRenderTargets(1, &back_buffer, app->swapChain()->GetDepthBufferDSV(),
                                         Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        if (state.scene.object == DeepSkyObject::Galaxy) {
            ImVec2 const size = ImGui::GetIO().DisplaySize;
            drawExplorerOverlay(state.galaxy_explorer, state.scene.galaxy, renderer.galaxy(),
                                ImGui::GetBackgroundDrawList(), size.x, size.y);
        }
        drawControlPanel(*app, state, metrics, frame_ms, scene_ms, post_ms, renderer.galaxy());
        app->renderImGui();
        app->present(state.v_sync);
        double const frame_time = milliseconds(frame_begin, Clock::now());
        frame_ms.push(frame_time);

        ++frame_count;
        if (options.smoke_frames > 0) {
            // A smoke run plays one object throughout (--object), or visits
            // each in turn, and keeps every frame time: a smooth run has a
            // worst frame close to its median.
            std::size_t const shown = static_cast<std::size_t>(state.scene.object);
            // The first frames of an object build its simulation.
            if (++smoke_shown[shown] > 3) {
                smoke_ms[shown].push(frame_time);
                // Frames far above the rest, and when they happened.
                if (frame_time > 20.0 && smoke_slow[shown].size() < 24) {
                    smoke_slow[shown].push_back(smoke_shown[shown]);
                    smoke_slow[shown].push_back(static_cast<int>(metrics.gas.steps));
                }
                if (metrics.gpu_sample_new) {
                    smoke_simulation_ms[shown].push(metrics.gpu_simulation_ms);
                    smoke_scene_ms[shown].push(metrics.gpu_scene_ms);
                }
            }
            if (!options.smoke_single) {
                int const per_object =
                    std::max(options.smoke_frames / static_cast<int>(kDeepSkyObjectCount), 1);
                if (frame_count % per_object == 0) {
                    selectObject(state, static_cast<DeepSkyObject>(
                                            (frame_count / per_object) %
                                            static_cast<int>(kDeepSkyObjectCount)));
                }
            }
            if (frame_count >= options.smoke_frames) {
                running = false;
            }
        }
    }
    if (options.smoke_frames > 0) {
        std::printf("DeepSkyLab smoke: %d hidden-window frame(s), v-sync %s, %u warning(s), %u "
                    "error(s)\n",
                    frame_count, state.v_sync ? "on" : "off", app->warningCount(), app->errorCount());
        for (std::size_t index = 0; index < kDeepSkyObjectCount; ++index) {
            RollingStats const& stats = smoke_ms[index];
            if (stats.empty()) {
                continue;
            }
            std::printf("  %-13s %5zu frames: median %7.3f ms, p95 %7.3f, p99 %7.3f, worst %7.3f | "
                        "GPU median: simulation %.2f ms, drawing %.2f ms\n",
                        kObjects[index].key, stats.size(), stats.percentile(50.0),
                        stats.percentile(95.0), stats.percentile(99.0), stats.maximum(),
                        smoke_simulation_ms[index].percentile(50.0),
                        smoke_scene_ms[index].percentile(50.0));
            if (!smoke_slow[index].empty()) {
                std::printf("    frames over 20 ms:");
                for (std::size_t slow = 0; slow + 1 < smoke_slow[index].size(); slow += 2) {
                    std::printf(" #%d (gas step %d)", smoke_slow[index][slow],
                                smoke_slow[index][slow + 1]);
                }
                std::printf("\n");
            }
        }
    }
    return app->errorCount() == 0 ? 0 : 2;
}

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

void printHelp() {
    std::printf(
        "Helios DeepSkyLab (real time, Diligent/Vulkan)\n\n"
        "Usage: helios_deep_sky_lab [route] [options]\n\n"
        "Routes (default: interactive window):\n"
        "  --capture PATH.png   Windowless render to a PNG\n"
        "  --benchmark          Windowless GPU timing run with a moving camera\n"
        "  --validate-gas       Windowless check of the gas solver against exact solutions\n"
        "  --smoke-frames N     Interactive route in a hidden window for N frames; prints\n"
        "                       frame-time statistics (of --object, or of each in turn)\n"
        "  --vsync on|off       Interactive route: wait for the display (default on)\n\n"
        "Object:\n"
        "  --object nebula|neutron-star|galaxy|remnant|black-hole|planet|pulsar   default nebula\n\n"
        "Image:\n"
        "  --width N --height N Output size, default 1920x1080\n"
        "  --supersample N      Render N times larger and filter down (1-4)\n"
        "  --accumulate N       Jittered frames averaged by --capture, default 48\n"
        "  --sequence N         With --capture: N frames PATH_000.png ... along\n"
        "  --sequence-orbit DEG   an azimuth sweep (split over the frames; 360 loops)\n"
        "  --sequence-time T      and/or a time advance in the object's unit;\n"
        "  --sequence-turns R     neutron star and pulsar: R rotations instead\n"
        "  --frames N           Benchmark frame count, default 180\n"
        "  --report PATH.json   Write a JSON report\n"
        "  --gas-profile PATH.csv  With --capture of a gas object: write shell averages of\n"
        "                       the simulation state around the centre of the box\n"
        "  --validation on|off  Vulkan validation layers (default on for --capture and\n"
        "                       --validate-gas, off for the window and --benchmark)\n\n"
        "Camera (orbits the object; distance in pc, M or kpc; for the planet: altitude in\n"
        "metres, latitude and longitude):\n"
        "  --distance X --elevation DEG --azimuth DEG --fov DEG\n\n"
        "Scene:\n"
        "  --time T             Scene time of the neutron star (M/c)\n"
        "  --seed N             Seed of the simulated object's initial conditions\n"
        "  --sim-time T         Simulate to this time: Myr for the galaxy (default 600; in the\n"
        "                       window it plays on to 13000) and the nebula (1.3), years for\n"
        "                       the remnant (560)\n"
        "  --volume N           Gas grid cells per edge, a multiple of 16 (default: the preset)\n"
        "  --box X              Gas box half-width in pc (default: the variant's)\n"
        "  --stretch A          Gas grid stretch, 0 = uniform (default: the variant's)\n"
        "  --clumpiness X       Remnant: rms density fluctuation of the ejecta (default 0.15)\n"
        "  --galaxy-stars N --galaxy-gas N --galaxy-bulge N   Particle counts\n"
        "  --galaxy-mesh N      Gravity mesh cells per edge, a power of two (default 512)\n"
        "  --galaxy-focus F     What the galaxy camera orbits: centre; the first star near 8 kpc\n"
        "                       that is sunlike, ringed (has a ringed giant), lava (has a lava\n"
        "                       world), giant or whitedwarf; or particle:N, star:N:R,\n"
        "                       planet:N:R:P, moon:N:R:P:M\n"
        "  --distance-au X      Camera distance in AU (galaxy)\n"
        "  --dust X             Galaxy: dust per unit gas, Milky Way = 1\n"
        "  --steps N            Ray-march samples, default per object\n"
        "  --stars X --band X   Background star and galactic band intensity\n"
        "  --variant NAME       Variant of the object (each panel lists them):\n"
        "                       nebula: orion, pillars, blister, reflection\n"
        "                       remnant: core-collapse, type-ia, crab, radiative\n"
        "                       galaxy: milky-way, barred, flocculent, early-type\n"
        "                       black-hole: cool-disk, stellar, supermassive, bare\n"
        "                       neutron-star: isolated, young-pulsar, millisecond-pulsar,\n"
        "                       magnetar, accreting, orthogonal, aligned\n"
        "                       pulsar: the rotation-powered ones of those\n"
        "  --xray               Neutron star, pulsar: show the X-ray band instead of visible light\n"
        "  --lorentz X          Neutron star, pulsar: Lorentz factor of the outflow (default 2.5)\n"
        "  --inclination DEG    Neutron star, pulsar: angle between magnetic and spin axes\n"
        "  --grid X             Neutron star, pulsar: surface graticule, 0-1\n"
        "  --preset performance|balanced|ultra   Quality of every object: gas grid (192, 256\n"
        "                       or 320 cells per edge), ray-march samples, galaxy particles,\n"
        "                       black-hole integration, neutron-star steps, planet detail;\n"
        "                       default balanced; options after it override its values\n"
        "  --turbulence X       Black hole: painted streaks of the disk, 0-0.95 (0 = none)\n"
        "  --look-down DEG      Planet: camera pitch below the horizon, 0-90\n"
        "  --rings on|off       Planet: the rings of the system (overrides the variant)\n"
        "  --clouds on|off      Planet: the cloud layer (overrides the variant)\n"
        "  --moons N            Planet: moons drawn, 0-4 (overrides the variant)\n"
        "  --descent SECONDS    Planet: place the camera this far into the scripted descent\n"
        "  --threads N          Worker threads for simulations (default: all cores)\n"
        "  --background         Run at low process priority\n"
        "  --exposure EV --glare X\n");
}

template <typename T> [[nodiscard]] std::optional<T> parseNumber(std::string_view text) {
    T value{};
    auto const [end, code] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (code != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] bool parseOptions(int argc, char** argv, Options& options, std::string& error) {
    LabState& state = options.state;
    DeepSkySettings& scene = state.scene;
    resetState(state);
    selectObject(state, DeepSkyObject::Nebula);
    std::optional<int> steps;
    std::optional<float> band;
    std::optional<double> descent_seconds;
    std::string variant;
    // Planet switches, applied after the variant so that they override it.
    std::optional<bool> rings;
    std::optional<bool> clouds;
    std::optional<std::uint32_t> moons;
    // Neutron-star options that apply on top of the chosen state.
    std::optional<float> lorentz;
    std::optional<float> inclination;
    std::optional<float> grid;
    // Gas-run overrides, applied after the variant.
    std::optional<double> box;
    std::optional<double> stretch;
    std::optional<double> clumpiness;
    applyQualityPreset(state, state.quality_preset, true);
    applyNeutronStarVariant(state, DeepSkyObject::NeutronStar, state.neutron_star_variant, true);
    applyNeutronStarVariant(state, DeepSkyObject::Pulsar, state.pulsar_variant, true);
    bool xray = false;
    bool has_exposure = false;

    int index = 1;
    auto value = [&](std::string_view option) -> std::optional<std::string_view> {
        if (index + 1 >= argc) {
            error = "missing value for " + std::string{option};
            return std::nullopt;
        }
        return std::string_view{argv[++index]};
    };
    auto number = [&](std::string_view option, double minimum, double maximum,
                      auto& target) -> bool {
        auto const text = value(option);
        if (!text) {
            return false;
        }
        auto const parsed = parseNumber<double>(*text);
        if (!parsed || !std::isfinite(*parsed) || *parsed < minimum || *parsed > maximum) {
            error = std::string{option} + " must be in [" + std::to_string(minimum) + ", " +
                    std::to_string(maximum) + "]";
            return false;
        }
        target = static_cast<std::remove_reference_t<decltype(target)>>(*parsed);
        return true;
    };
    // Camera options apply to whichever object ends up selected.
    OrbitCamera override_camera{};
    bool has_distance = false;
    bool has_elevation = false;
    bool has_azimuth = false;
    bool has_fov = false;

    for (; index < argc; ++index) {
        std::string_view const argument = argv[index];
        bool ok = true;
        if (argument == "--help" || argument == "-h") {
            options.show_help = true;
        } else if (argument == "--capture") {
            auto const path = value(argument);
            ok = path.has_value() && !path->empty();
            if (ok) {
                options.mode = Mode::Capture;
                options.capture_path = std::string{*path};
            }
        } else if (argument == "--benchmark") {
            options.mode = Mode::Benchmark;
        } else if (argument == "--validate-gas") {
            options.mode = Mode::ValidateGas;
        } else if (argument == "--smoke-frames") {
            ok = number(argument, 1.0, 100000.0, options.smoke_frames);
        } else if (argument == "--vsync") {
            auto const text = value(argument);
            ok = text.has_value() && (*text == "on" || *text == "off");
            if (ok) {
                state.v_sync = *text == "on";
            } else if (error.empty()) {
                error = "--vsync must be on or off";
            }
        } else if (argument == "--object") {
            options.smoke_single = true;
            auto const text = value(argument);
            ok = false;
            for (std::size_t object = 0; text && object < kDeepSkyObjectCount; ++object) {
                if (*text == kObjects[object].key) {
                    selectObject(state, static_cast<DeepSkyObject>(object));
                    ok = true;
                }
            }
            if (!ok && error.empty()) {
                error = "--object must be nebula, neutron-star, galaxy, remnant, black-hole, planet "
                        "or pulsar";
            }
        } else if (argument == "--gas-profile") {
            auto const path = value(argument);
            ok = path.has_value();
            if (ok) {
                options.profile_path = std::string{*path};
            }
        } else if (argument == "--report") {
            auto const path = value(argument);
            ok = path.has_value();
            if (ok) {
                options.report_path = std::string{*path};
            }
        } else if (argument == "--validation") {
            auto const text = value(argument);
            ok = text.has_value() && (*text == "on" || *text == "off");
            if (ok) {
                options.validation_layers = *text == "on";
            } else if (error.empty()) {
                error = "--validation must be on or off";
            }
        } else if (argument == "--width") {
            ok = number(argument, 16.0, 8192.0, options.width);
        } else if (argument == "--height") {
            ok = number(argument, 16.0, 8192.0, options.height);
        } else if (argument == "--supersample") {
            ok = number(argument, 1.0, 4.0, options.supersample);
        } else if (argument == "--accumulate") {
            ok = number(argument, 1.0, 1024.0, options.accumulate);
        } else if (argument == "--sequence") {
            ok = number(argument, 1.0, 100000.0, options.sequence_frames);
        } else if (argument == "--sequence-orbit") {
            ok = number(argument, -3600.0, 3600.0, options.sequence_orbit_deg);
        } else if (argument == "--sequence-time") {
            ok = number(argument, -1.0e12, 1.0e12, options.sequence_time);
        } else if (argument == "--sequence-turns") {
            ok = number(argument, 0.0, 1000.0, options.sequence_turns);
        } else if (argument == "--frames") {
            ok = number(argument, 1.0, 100000.0, options.frames);
        } else if (argument == "--preset") {
            auto const text = value(argument);
            ok = false;
            for (std::size_t preset = 0; text && preset < kQualityPresets.size(); ++preset) {
                std::string name{kQualityPresets[preset]};
                name[0] = static_cast<char>(name[0] - 'A' + 'a');
                if (*text == name) {
                    applyQualityPreset(state, static_cast<int>(preset), true);
                    ok = true;
                }
            }
            if (!ok && error.empty()) {
                error = "--preset must be performance, balanced or ultra";
            }
        } else if (argument == "--turbulence") {
            ok = number(argument, 0.0, 0.95, scene.black_hole.disk_turbulence);
        } else if (argument == "--look-down") {
            ok = number(argument, 0.0, 90.0, state.planet_look_down_deg);
        } else if (argument == "--descent") {
            double parsed = 0.0;
            ok = number(argument, 0.0, 1000.0, parsed);
            descent_seconds = parsed;
        } else if (argument == "--distance") {
            ok = number(argument, 1.0e-16, 1.0e9, override_camera.distance);
            has_distance = ok;
            options.galaxy_has_distance = ok;
        } else if (argument == "--distance-au") {
            double au = 0.0;
            ok = number(argument, 1.0e-9, 1.0e9, au);
            override_camera.distance = au / 2.06264806e8;
            has_distance = ok;
            options.galaxy_has_distance = ok;
        } else if (argument == "--galaxy-focus") {
            auto const text = value(argument);
            ok = text.has_value();
            if (ok) {
                std::string_view const spec = *text;
                GalaxyFocus focus;
                std::array<unsigned long, 4> numbers{};
                int parsed = 0;
                auto const colon = spec.find(':');
                std::string_view const kind = spec.substr(0, colon);
                if (colon != std::string_view::npos) {
                    std::string_view rest = spec.substr(colon + 1U);
                    while (!rest.empty() && parsed < 4) {
                        auto const next = rest.find(':');
                        auto const number_text = rest.substr(0, next);
                        auto const parsed_number = parseNumber<unsigned long>(number_text);
                        if (!parsed_number) {
                            ok = false;
                            break;
                        }
                        numbers[static_cast<std::size_t>(parsed++)] = *parsed_number;
                        rest = next == std::string_view::npos ? std::string_view{} : rest.substr(next + 1U);
                    }
                }
                if (kind == "centre" || kind == "center") {
                    focus.kind = GalaxyFocus::Kind::Centre;
                } else if (kind == "sunlike") {
                    options.galaxy_search = StarSearch::SunLike;
                } else if (kind == "ringed") {
                    options.galaxy_search = StarSearch::RingedGiant;
                } else if (kind == "lava") {
                    options.galaxy_search = StarSearch::LavaWorld;
                } else if (kind == "giant") {
                    options.galaxy_search = StarSearch::Giant;
                } else if (kind == "whitedwarf") {
                    options.galaxy_search = StarSearch::WhiteDwarf;
                } else if (kind == "particle" && parsed == 1) {
                    focus.kind = GalaxyFocus::Kind::Particle;
                } else if (kind == "star" && parsed == 2) {
                    focus.kind = GalaxyFocus::Kind::Star;
                } else if (kind == "planet" && parsed == 3) {
                    focus.kind = GalaxyFocus::Kind::Planet;
                } else if (kind == "moon" && parsed == 4) {
                    focus.kind = GalaxyFocus::Kind::Moon;
                } else {
                    ok = false;
                }
                focus.particle = static_cast<std::uint32_t>(numbers[0]);
                focus.rank = static_cast<std::uint32_t>(numbers[1]);
                focus.planet = static_cast<std::uint32_t>(numbers[2]);
                focus.moon = static_cast<std::uint32_t>(numbers[3]);
                scene.galaxy.focus = focus;
                scene.galaxy.previous_focus = focus;
                scene.galaxy.transition = 1.0f;
                if (!ok && error.empty()) {
                    error = "--galaxy-focus must be centre, sunlike, ringed, lava, giant, whitedwarf, "
                            "particle:N, star:N:R, planet:N:R:P or moon:N:R:P:M";
                }
            }
        } else if (argument == "--elevation") {
            ok = number(argument, -89.5, 89.5, override_camera.elevation_deg);
            has_elevation = ok;
        } else if (argument == "--azimuth") {
            ok = number(argument, -720.0, 720.0, override_camera.azimuth_deg);
            has_azimuth = ok;
        } else if (argument == "--fov") {
            ok = number(argument, 5.0, 110.0, override_camera.vertical_fov_deg);
            has_fov = ok;
        } else if (argument == "--time") {
            ok = number(argument, -1.0e9, 1.0e9, scene.time);
        } else if (argument == "--seed") {
            ok = number(argument, 0.0, 4.0e9, scene.nebula.recipe.seed);
            scene.galaxy.recipe.seed = scene.nebula.recipe.seed;
            scene.remnant.recipe.seed = scene.nebula.recipe.seed;
        } else if (argument == "--sim-time") {
            double parsed = 0.0;
            ok = number(argument, 0.0, 16000.0, parsed);
            options.sim_time = parsed;
        } else if (argument == "--volume") {
            std::uint32_t parsed = 0;
            ok = number(argument, 32.0, 512.0, parsed) && parsed % 16U == 0;
            scene.nebula.recipe.resolution = parsed;
            scene.remnant.recipe.resolution = parsed;
            options.volume = parsed;
        } else if (argument == "--box") {
            double parsed = 0.0;
            ok = number(argument, 0.5, 100.0, parsed);
            box = parsed;
        } else if (argument == "--stretch") {
            double parsed = 0.0;
            ok = number(argument, 0.0, 4.0, parsed);
            stretch = parsed;
        } else if (argument == "--clumpiness") {
            double parsed = 0.0;
            ok = number(argument, 0.0, 0.5, parsed);
            clumpiness = parsed;
        } else if (argument == "--galaxy-stars") {
            ok = number(argument, 1000.0, 8.0e6, scene.galaxy.recipe.disk_stars);
        } else if (argument == "--galaxy-gas") {
            ok = number(argument, 1000.0, 4.0e6, scene.galaxy.recipe.gas_clouds);
        } else if (argument == "--galaxy-bulge") {
            ok = number(argument, 1000.0, 2.0e6, scene.galaxy.recipe.bulge_stars);
        } else if (argument == "--galaxy-mesh") {
            ok = number(argument, 64.0, 2048.0, scene.galaxy.recipe.grid);
        } else if (argument == "--dust") {
            ok = number(argument, 0.0, 10.0, scene.galaxy.dust);
        } else if (argument == "--steps") {
            int parsed = 0;
            ok = number(argument, 8.0, 512.0, parsed);
            steps = parsed;
        } else if (argument == "--stars") {
            ok = number(argument, 0.0, 16.0, scene.star_intensity);
        } else if (argument == "--band") {
            float parsed = 0.0f;
            ok = number(argument, 0.0, 16.0, parsed);
            band = parsed;
        } else if (argument == "--variant") {
            auto const text = value(argument);
            ok = text.has_value();
            if (ok) {
                variant = std::string{*text};
            }
        } else if (argument == "--xray") {
            xray = true;
        } else if (argument == "--lorentz") {
            float parsed = 0.0f;
            ok = number(argument, 1.0, 100.0, parsed);
            lorentz = parsed;
        } else if (argument == "--inclination") {
            float parsed = 0.0f;
            ok = number(argument, 0.0, 90.0, parsed);
            inclination = parsed;
        } else if (argument == "--grid") {
            float parsed = 0.0f;
            ok = number(argument, 0.0, 1.0, parsed);
            grid = parsed;
        } else if (argument == "--threads") {
            ok = number(argument, 1.0, 256.0, options.threads);
        } else if (argument == "--background") {
            options.low_priority = true;
        } else if (argument == "--exposure") {
            ok = number(argument, -8.0, 8.0, scene.exposure_ev);
            has_exposure = ok;
        } else if (argument == "--glare") {
            ok = number(argument, 0.0, 1.0, scene.glare);
        } else if (argument == "--rings") {
            auto const text = value(argument);
            ok = text.has_value() && (*text == "on" || *text == "off");
            if (ok) {
                rings = *text == "on";
            }
        } else if (argument == "--clouds") {
            auto const text = value(argument);
            ok = text.has_value() && (*text == "on" || *text == "off");
            if (ok) {
                clouds = *text == "on";
            }
        } else if (argument == "--moons") {
            double count = 0.0;
            ok = number(argument, 0.0, static_cast<double>(kMaxPlanetMoons), count);
            if (ok) {
                moons = static_cast<std::uint32_t>(count);
            }
        } else {
            error = "unknown option: " + std::string{argument};
            ok = false;
        }
        if (!ok) {
            if (error.empty()) {
                error = "invalid value for " + std::string{argument};
            }
            return false;
        }
    }

    if (!variant.empty()) {
        bool found = false;
        if (isNeutronStar(scene.object)) {
            for (std::size_t choice = 0; choice < kNeutronStarVariantCount; ++choice) {
                auto const kind = static_cast<NeutronStarVariant>(choice);
                bool const offered =
                    scene.object == DeepSkyObject::NeutronStar ||
                    std::find(kPulsarVariants.begin(), kPulsarVariants.end(), kind) !=
                        kPulsarVariants.end();
                if (offered && variant == neutronStarVariantInfo(kind).key) {
                    applyNeutronStarVariant(state, scene.object, static_cast<int>(choice), true);
                    found = true;
                }
            }
        } else {
            auto const variants = variantsOf(scene.object);
            for (std::size_t choice = 0; choice < variants.size(); ++choice) {
                if (variant == variants[choice].key) {
                    applyObjectVariant(state, scene.object, static_cast<int>(choice), true,
                                       !has_exposure);
                    found = true;
                }
            }
        }
        if (!found) {
            error = "--variant " + variant + " is not a state of " + infoFor(scene.object).key;
            return false;
        }
    }
    if (rings) {
        scene.planet.system.rings.enabled = *rings;
    }
    if (clouds) {
        scene.planet.system.clouds.enabled = *clouds;
    }
    if (moons) {
        scene.planet.system.moon_count = *moons;
    }
    if (box) {
        scene.nebula.recipe.half_extent_pc = *box;
        scene.remnant.recipe.half_extent_pc = *box;
    }
    if (stretch) {
        scene.nebula.recipe.stretch = *stretch;
        scene.remnant.recipe.stretch = *stretch;
    }
    if (clumpiness) {
        scene.remnant.recipe.clumpiness = *clumpiness;
    }
    if (isNeutronStar(scene.object)) {
        NeutronStarSettings& star = starSettings(state, scene.object);
        if (xray) {
            star.band = NeutronStarBand::XRay;
        }
        if (lorentz) {
            star.lorentz_factor = *lorentz;
        }
        if (inclination) {
            star.magnetic_inclination_deg = *inclination;
        }
        if (grid) {
            star.grid = *grid;
        }
    }
    if (descent_seconds) {
        // The descent sets the planet's camera; explicit camera options
        // below still override it.
        OrbitCamera& planet = state.orbits[static_cast<std::size_t>(DeepSkyObject::Planet)];
        PlanetCamera camera;
        state.planet_route.restart(camera);
        state.planet_route.advance(*descent_seconds, camera);
        state.planet_route.playing = false;
        planet.distance = camera.altitude_agl_m;
        planet.elevation_deg = camera.latitude_deg;
        planet.azimuth_deg = camera.longitude_deg;
        state.planet_look_down_deg = camera.look_down_deg;
    }
    OrbitCamera& orbit = state.orbits[static_cast<std::size_t>(scene.object)];
    if (has_distance) {
        orbit.distance = override_camera.distance;
    }
    if (has_elevation) {
        orbit.elevation_deg = override_camera.elevation_deg;
    }
    if (has_azimuth) {
        orbit.azimuth_deg = override_camera.azimuth_deg;
    }
    if (has_fov) {
        orbit.vertical_fov_deg = override_camera.vertical_fov_deg;
    }
    if (steps) {
        scene.steps = *steps;
    }
    if (band) {
        scene.band_intensity = *band;
    }
    state.pending_nebula = scene.nebula.recipe;
    state.pending_remnant = scene.remnant.recipe;
    state.pending_galaxy = scene.galaxy.recipe;
    scene.galaxy.time_myr = kDefaultGalaxyMyr;
    if (options.sim_time) {
        switch (scene.object) {
        case DeepSkyObject::Galaxy:
            scene.galaxy.time_myr = *options.sim_time;
            state.galaxy_run_to_myr = *options.sim_time;
            break;
        case DeepSkyObject::Nebula:
            scene.nebula.time_myr = *options.sim_time;
            state.nebula_run_to_myr = *options.sim_time;
            break;
        case DeepSkyObject::SupernovaRemnant:
            scene.remnant.time_years = *options.sim_time;
            state.remnant_run_to_years = *options.sim_time;
            break;
        case DeepSkyObject::NeutronStar:
        case DeepSkyObject::Pulsar:
        case DeepSkyObject::BlackHole:
        case DeepSkyObject::Planet:
            break;
        }
    }
    return true;
}

} // namespace
} // namespace helios::examples

int main(int argc, char** argv) {
    using namespace helios::examples;

    Options options;
    std::string error;
    if (!parseOptions(argc, argv, options, error)) {
        std::fprintf(stderr, "DeepSkyLab: %s\n", error.c_str());
        return 2;
    }
    if (options.show_help) {
        printHelp();
        return 0;
    }
#ifdef _WIN32
    if (options.low_priority) {
        // Simulations use every core they are given; this keeps them out of
        // the way of whatever else the machine is doing.
        SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    }
#endif
    switch (options.mode) {
    case Mode::Capture:
        return runCapture(options);
    case Mode::Benchmark:
        return runBenchmark(options);
    case Mode::ValidateGas:
        return runGasValidation(options.volume, options.report_path,
                                options.validation_layers.value_or(true));
    case Mode::Interactive:
        break;
    }
    return runInteractive(options);
}

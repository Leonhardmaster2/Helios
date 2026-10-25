// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "gas_scene.hpp"

#include "stellar_population.hpp"

#include "helios/blackhole/blackbody.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>

namespace helios::examples {
namespace {

using Row = std::array<float, 4>;

// Time steps per batch while a capture waits for a run to reach its target.
constexpr std::uint32_t kWaitBatch = 16;

[[nodiscard]] Row row(double x, double y, double z, double w) {
    return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), static_cast<float>(w)};
}

[[nodiscard]] std::uint64_t mix(std::uint64_t z) {
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

[[nodiscard]] double uniform(std::uint64_t seed, std::uint64_t index, std::uint64_t stream) {
    std::uint64_t const h = mix(mix(seed * 0xD1B54A32D192ED03ULL + stream) ^ (index * 0x8CB92BA72F3D8DD7ULL));
    return (static_cast<double>(h >> 11) + 0.5) * (1.0 / 9007199254740992.0);
}

// Linear sRGB of a set of emission lines, clipped to the gamut.
struct Line {
    double wavelength_nm;
    double strength;
};

template <std::size_t N> [[nodiscard]] math::Vec3d lineLight(std::array<Line, N> const& lines) {
    math::Vec3d sum{};
    for (Line const& line : lines) {
        sum += blackhole::monochromaticLinearSrgb(line.wavelength_nm) * line.strength;
    }
    return {std::max(sum[0], 0.0), std::max(sum[1], 0.0), std::max(sum[2], 0.0)};
}

// Line strengths relative to H-beta.  Balmer lines: case B recombination.
// [O III] where the ionisation parameter is high, [N II] and [S II] where it
// is low: the values bracket what H II regions of solar composition show.
constexpr std::array<Line, 2> kBalmerLines{{{656.28, 2.86}, {486.13, 1.0}}};
constexpr std::array<Line, 1> kOxygenLines{{{500.7, 4.0}}};
constexpr std::array<Line, 2> kLowLines{{{658.35, 1.4}, {672.4, 0.9}}};

// ---- nebula -------------------------------------------------------------------

constexpr double kNeutralSound = 0.35;  // km/s: molecular gas at 30 K
constexpr double kIonisedSound = 12.85; // km/s: ionised hydrogen at 10^4 K
// Stromgren integral Q / (4 pi alpha_B) per 1e46 photons/s, in pc^3 cm^-6
// (case B recombination at 10^4 K).
constexpr double kStromgrenPerRate = 104.6;
// Visual optical depth of a column of one hydrogen atom per cm^3 over one
// parsec with the Milky Way's dust: N_H / (2.07e21 cm^-2).
constexpr double kDustPerColumn = 1.49e-3;
constexpr double kNebulaDustUnit = 1.0e-3; // the volume stores density / 1000
// Display scale of the line emission per (cm^-6 pc) and of scattered
// starlight per (cm^-3 pc) at one parsec from the star.
constexpr double kNebulaLineScale = 8.0e-6;
constexpr double kNebulaScatterScale = 2.5e-3;
// Ionisation parameter at which half of the oxygen is doubly ionised.
constexpr double kOxygenBoundary = 1.0e-3;

[[nodiscard]] HydroSetup nebulaSetup(NebulaRecipe const& recipe) {
    HydroSetup setup;
    setup.scenario = HydroScenario::Nebula;
    setup.resolution = recipe.resolution;
    setup.seed = recipe.seed;
    setup.half_extent = recipe.half_extent_pc;
    setup.stretch = std::clamp(recipe.stretch, 0.0, 4.0);
    setup.gamma = 5.0 / 3.0;
    setup.density_floor = 1.0e-3;
    setup.pressure_floor = 0.01 * kNeutralSound * kNeutralSound;
    setup.scenario_z = static_cast<float>(kNeutralSound);
    setup.scenario_w = static_cast<float>(kIonisedSound);
    // The star sits a little off the cloud's centre; it is off until ignition.
    double const offset = 0.12 * recipe.cloud_radius_pc;
    double const along = std::clamp(recipe.star_offset, 0.0, 1.0) * recipe.cloud_radius_pc;
    setup.source[0] = row(along + offset * (2.0 * uniform(recipe.seed, 1, 900) - 1.0),
                          offset * (2.0 * uniform(recipe.seed, 2, 900) - 1.0),
                          offset * (2.0 * uniform(recipe.seed, 3, 900) - 1.0), 0.0);
    setup.source[1] = row(recipe.cloud_density, std::max(recipe.ambient_density, 0.01),
                          recipe.cloud_radius_pc, recipe.turbulent_speed_km_s / std::sqrt(3.0));
    // The ray grid reaches the far corner of the box from anywhere the star
    // can be.
    setup.source[2] =
        row(recipe.half_extent_pc * std::sqrt(3.0) + 2.0 * offset + along, 0.0, 0.0, 0.0);
    return setup;
}

void nebulaShade(HydroSetup& setup, NebulaRecipe const& recipe) {
    math::Vec3d const balmer = lineLight(kBalmerLines) * kNebulaLineScale;
    math::Vec3d const oxygen = lineLight(kOxygenLines) * kNebulaLineScale;
    math::Vec3d const low = lineLight(kLowLines) * kNebulaLineScale;
    setup.shade[0] = row(balmer[0], balmer[1], balmer[2], 0.0);
    setup.shade[1] = row(oxygen[0], oxygen[1], oxygen[2], kOxygenBoundary);
    setup.shade[2] = row(low[0], low[1], low[2], 0.0);
    // Small grains scatter blue light best: the colour of a reflection
    // nebula.  The scale is calibrated for the light of a 40 solar-mass star.
    double const scatter = kNebulaScatterScale *
                           mainSequenceStar(recipe.massive_star_solar).luminosity /
                           mainSequenceStar(40.0).luminosity;
    setup.shade[3] = row(0.52 * scatter, 0.72 * scatter, 1.0 * scatter, kDustPerColumn);
    setup.shade[4] = row(kNebulaDustUnit, 6.0e4, 0.0, 0.0);
}

// The cluster: the ionising star and lower-mass stars around it, with masses
// drawn from the upper part of the Kroupa mass function.
[[nodiscard]] std::vector<GasPointSource> makeCluster(NebulaRecipe const& recipe,
                                                      HydroSetup const& setup) {
    std::vector<GasPointSource> cluster;
    math::Vec3d const centre{setup.source[0][0], setup.source[0][1], setup.source[0][2]};
    StarLight const massive = mainSequenceStar(recipe.massive_star_solar);
    cluster.push_back({centre, massive.temperature_k, massive.luminosity});
    constexpr double kLowest = 1.5;
    constexpr double kHighest = 18.0;
    constexpr double kScaleRadius = 0.45; // pc, a Plummer sphere
    for (std::uint32_t index = 0; index < recipe.cluster_stars; ++index) {
        // Inverse transform of dN/dM ~ M^-2.3.
        double const a = std::pow(kLowest, -1.3);
        double const b = std::pow(kHighest, -1.3);
        double const mass = std::pow(a + uniform(recipe.seed, index, 910) * (b - a), -1.0 / 1.3);
        double const u = std::clamp(uniform(recipe.seed, index, 911), 1.0e-3, 0.97);
        double const radius = kScaleRadius / std::sqrt(std::pow(u, -2.0 / 3.0) - 1.0);
        double const cosine = 2.0 * uniform(recipe.seed, index, 912) - 1.0;
        double const sine = std::sqrt(std::max(1.0 - cosine * cosine, 0.0));
        double const angle = 2.0 * std::numbers::pi * uniform(recipe.seed, index, 913);
        StarLight const light = mainSequenceStar(mass);
        cluster.push_back({centre + math::Vec3d{radius * sine * std::cos(angle),
                                                radius * sine * std::sin(angle), radius * cosine},
                           light.temperature_k, light.luminosity});
    }
    return cluster;
}

// ---- remnant ------------------------------------------------------------------

constexpr double kEnergyUnitErg = 1.989e49;    // one solar mass at 1000 km/s, squared
constexpr double kPowerUnitErgPerS = 6.445e38; // that energy per 977.8 yr
// Solar masses per pc^3 of gas with one hydrogen atom per cm^3 (and helium).
constexpr double kSolarPerAtom = 0.0345;
constexpr double kAmbientSound2 = 1.0e-4;      // (10 km/s)^2
// Sound speed squared that radiating ejecta are held at: (77 km/s)^2.  Real
// ejecta cool to 10^4 K, (12 km/s)^2, and are compressed into knots far
// smaller than a cell; this floor keeps them resolved.
constexpr double kCoolSound2 = 6.0e-3;
// Pressure / density of gas at about 10^6 K: hotter gas of this density takes
// longer than the remnant's age to cool.
constexpr double kCoolBelow = 0.04;            // (200 km/s)^2
// Speed the wind's energy would give its mass.  The real wind is
// ultra-relativistic; this keeps it light and fast compared with the ejecta.
constexpr double kWindSpeed = 12.0;
constexpr double kInjectionCells = 6.0;
// Display scales: line emission per (solar mass / pc^3)^2 per pc,
// synchrotron light per pc at the reference pressure, thermal light of hot
// plasma per (density^2 / sound speed) per pc, and the light of the blast
// wave per unit density of the gas it is crossing.
constexpr double kRemnantLineScale = 0.06;
constexpr double kRemnantSynchrotronScale = 0.22;
constexpr double kReferencePressure = 0.04;
constexpr double kRemnantPlasmaScale = 0.6;
constexpr double kRemnantRimScale = 1.2;
// Ejecta density below which the gas is highly ionised, solar masses / pc^3:
// where the ionisation parameter of the bubble's synchrotron radiation
// (about 3e47 ionising photons per second, a parsec away) exceeds 1e-3.
constexpr double kRemnantZoneDensity = 1.4;
// Visual optical depth per (solar mass / pc^2) of gas with interstellar dust.
constexpr double kRemnantDustOpacity = 0.06;

[[nodiscard]] double pulsarPower(RemnantRecipe const& recipe, double time) {
    double const initial = recipe.pulsar_power_1e38_erg_s * 1.0e38 / kPowerUnitErgPerS;
    double const tau = recipe.spin_down_years / kRemnantTimeUnitYears;
    double const index = (recipe.braking_index + 1.0) / (recipe.braking_index - 1.0);
    return initial * std::pow(1.0 + time / tau, -index);
}

[[nodiscard]] HydroSetup remnantSetup(RemnantRecipe const& recipe) {
    HydroSetup setup;
    setup.scenario = HydroScenario::Remnant;
    setup.resolution = recipe.resolution;
    setup.seed = recipe.seed;
    setup.half_extent = recipe.half_extent_pc;
    setup.stretch = std::clamp(recipe.stretch, 0.0, 4.0);
    setup.gamma = 4.0 / 3.0; // the wind bubble is a relativistic gas
    setup.density_floor = 1.0e-6;
    setup.pressure_floor = 1.0e-6;
    setup.start_time = recipe.start_years / kRemnantTimeUnitYears;
    setup.scenario_z = static_cast<float>(kCoolSound2);
    setup.scenario_w = static_cast<float>(kCoolBelow);

    // Chevalier's (1982) ejecta: a flat core inside an envelope falling as
    // v^-n, fixed by the mass and kinetic energy of the explosion.
    double const energy = recipe.explosion_energy_1e51_erg * 1.0e51 / kEnergyUnitErg;
    double const mass = recipe.ejecta_mass_solar;
    double const n = std::clamp(recipe.envelope_index, 5.5, 14.0);
    double const core_velocity = std::sqrt(10.0 * energy * (n - 5.0) / (3.0 * mass * (n - 3.0)));
    double const core_density =
        3.0 * mass * (n - 3.0) /
        (4.0 * std::numbers::pi * n * core_velocity * core_velocity * core_velocity *
         setup.start_time * setup.start_time * setup.start_time);
    // The injection region spans a few of the central cells.
    double const cell = 2.0 * recipe.half_extent_pc / recipe.resolution *
                        (setup.stretch > 0.0 ? setup.stretch / std::sinh(setup.stretch) : 1.0);
    setup.source[0] = row(kInjectionCells * cell, 0.0, 0.0, 0.0);
    setup.source[1] = row(setup.start_time, core_velocity, core_density, n);
    setup.source[2] = row(recipe.ambient_density * kSolarPerAtom, kAmbientSound2, recipe.clumpiness,
                          2.5 * core_velocity);
    // Energy the pulsar has put out before the start, from the integral of
    // its spin-down law, in a bubble inside the ejecta's core.
    double const initial_power = recipe.pulsar_power_1e38_erg_s * 1.0e38 / kPowerUnitErgPerS;
    double const tau = recipe.spin_down_years / kRemnantTimeUnitYears;
    double const index = (recipe.braking_index + 1.0) / (recipe.braking_index - 1.0);
    double const prior_energy =
        initial_power * tau / (index - 1.0) *
        (1.0 - std::pow(1.0 + setup.start_time / tau, 1.0 - index));
    double const bubble = recipe.pulsar_power_1e38_erg_s > 0.0 ? 0.6 * core_velocity * setup.start_time : 0.0;
    setup.source[3] = row(recipe.radiative ? 1.0 : 0.0, prior_energy, bubble, kWindSpeed);
    return setup;
}

void remnantShade(HydroSetup& setup, RemnantSettings const& settings) {
    double const lines = kRemnantLineScale * std::max(settings.filaments, 0.0f);
    math::Vec3d const balmer = lineLight(kBalmerLines) * lines;
    math::Vec3d const oxygen = lineLight(kOxygenLines) * lines;
    math::Vec3d const low = lineLight(kLowLines) * lines;
    setup.shade[0] = row(balmer[0], balmer[1], balmer[2], 0.0);
    setup.shade[1] = row(oxygen[0], oxygen[1], oxygen[2], kRemnantZoneDensity);
    setup.shade[2] = row(low[0], low[1], low[2], 0.0);
    // A flat synchrotron spectrum has about the colour of a 12 000 K star.
    math::Vec3d const blue = blackhole::blackbodyColor(12000.0).linearSrgb;
    double const luminance = 0.2126729 * blue[0] + 0.7151522 * blue[1] + 0.0721750 * blue[2];
    double const synchrotron =
        kRemnantSynchrotronScale * std::max(settings.synchrotron, 0.0f) / luminance;
    setup.shade[3] = row(blue[0] * synchrotron, blue[1] * synchrotron, blue[2] * synchrotron,
                         1.0 / kReferencePressure);
    setup.shade[4] = row(1.0, 6.0e4, 0.0, 0.0);
    // Hot plasma radiates a flat spectrum in the visible (bremsstrahlung):
    // about the colour of a 10 000 K star.
    math::Vec3d const flat = blackhole::blackbodyColor(10000.0).linearSrgb;
    double const flat_luminance = 0.2126729 * flat[0] + 0.7151522 * flat[1] + 0.0721750 * flat[2];
    double const plasma = kRemnantPlasmaScale * std::max(settings.filaments, 0.0f) / flat_luminance;
    setup.shade[5] = row(flat[0] * plasma, flat[1] * plasma, flat[2] * plasma,
                         kRemnantRimScale / kRemnantLineScale);
}

} // namespace

bool GasScene::initialize(Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                          std::string& error) {
    return simulation_.initialize(device, context, error);
}

double GasScene::dustOpacity() const noexcept {
    return is_remnant_ ? kRemnantDustOpacity : kDustPerColumn / kNebulaDustUnit;
}

math::Vec3d GasScene::ambientScatter() const noexcept {
    HydroSetup const& setup = simulation_.setup();
    if (is_remnant_ || !simulation_.started() || setup.source[0][3] <= 0.0f) {
        return {};
    }
    double const ambient = setup.source[1][1];
    return math::Vec3d{setup.shade[3][0], setup.shade[3][1], setup.shade[3][2]} * ambient;
}

math::Vec3d GasScene::starPosition() const noexcept {
    HydroSetup const& setup = simulation_.setup();
    return {setup.source[0][0], setup.source[0][1], setup.source[0][2]};
}

bool GasScene::run(double target, double signal_floor, std::uint32_t steps, bool synchronous,
                   bool& changed, std::string& error) {
    auto const begin = std::chrono::steady_clock::now();
    std::uint64_t const before = simulation_.steps();
    if (!simulation_.advance(steps, target, signal_floor, synchronous, error)) {
        return false;
    }
    if (simulation_.steps() != before) {
        status_.last_advance_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        status_.last_advance_steps = static_cast<std::uint32_t>(simulation_.steps() - before);
        changed = true;
    }
    return true;
}

bool GasScene::update(NebulaSettings const& settings, std::uint32_t threads, std::uint32_t steps,
                      bool wait, bool& changed, std::string& error) {
    changed = false;
    double const target = settings.time_myr / kNebulaTimeUnitMyr;
    if (!simulation_.started() || is_remnant_ || !(nebula_ == settings.recipe) ||
        target < simulation_.time() - 1.0e-9) {
        auto const begin = std::chrono::steady_clock::now();
        HydroSetup setup = nebulaSetup(settings.recipe);
        nebulaShade(setup, settings.recipe);
        if (!simulation_.start(setup, threads, error)) {
            return false;
        }
        simulation_.traceRays();
        nebula_ = settings.recipe;
        is_remnant_ = false;
        cluster_ = makeCluster(settings.recipe, setup);
        visible_.clear();
        status_ = {};
        status_.setup_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        changed = true;
    }

    // Until ignition only the turbulence acts; then the star switches on.
    double const ignition = settings.recipe.ignition_myr / kNebulaTimeUnitMyr;
    HydroSetup& setup = simulation_.setup();
    if (setup.source[0][3] <= 0.0f) {
        double const until = std::min(target, ignition);
        do {
            if (!run(until, 0.0, wait ? kWaitBatch : steps, wait, changed, error)) {
                return false;
            }
        } while (wait && simulation_.time() < until);
        if (simulation_.time() >= ignition - 1.0e-9 && target > ignition) {
            double const rate = std::pow(10.0, settings.recipe.ionising_rate_log10 - 46.0);
            setup.source[0][3] = static_cast<float>(kStromgrenPerRate * rate);
            simulation_.traceRays();
            visible_ = cluster_;
            changed = true;
        }
    }
    if (setup.source[0][3] > 0.0f) {
        // Ionised gas appears at once where the photons reach, faster than the
        // measured signal speed can anticipate.
        do {
            if (!run(target, 1.5 * kIonisedSound, wait ? kWaitBatch : steps, wait, changed, error)) {
                return false;
            }
        } while (wait && simulation_.time() < target);
    }
    if (changed || !(nebula_shaded_ == settings)) {
        simulation_.shade();
        nebula_shaded_ = settings;
        changed = true;
    }
    status_.time = simulation_.time() * kNebulaTimeUnitMyr;
    status_.time_step = simulation_.timeStep() * kNebulaTimeUnitMyr;
    status_.steps = simulation_.steps();
    status_.resolution = simulation_.resolution();
    // Hydrogen atoms per cm^3 times pc^3, in solar masses (with helium).
    status_.mass = simulation_.totals().mass * 0.0345;
    status_.source_on = setup.source[0][3] > 0.0f;
    return true;
}

bool GasScene::update(RemnantSettings const& settings, std::uint32_t threads, std::uint32_t steps,
                      bool wait, bool& changed, std::string& error) {
    changed = false;
    double const target = settings.time_years / kRemnantTimeUnitYears;
    if (!simulation_.started() || !is_remnant_ || !(remnant_ == settings.recipe) ||
        target < simulation_.time() - 1.0e-9) {
        auto const begin = std::chrono::steady_clock::now();
        HydroSetup setup = remnantSetup(settings.recipe);
        remnantShade(setup, settings);
        if (!simulation_.start(setup, threads, error)) {
            return false;
        }
        remnant_ = settings.recipe;
        is_remnant_ = true;
        // The pulsar, if there is one: a hot point source at the centre.
        visible_.clear();
        if (settings.recipe.pulsar_power_1e38_erg_s > 0.0) {
            visible_.assign(1, GasPointSource{math::Vec3d{}, 12000.0, 1.0});
        }
        status_ = {};
        status_.setup_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        changed = true;
    }

    // The pulsar spins down; its wind carries the energy at a fixed speed.
    HydroSetup& setup = simulation_.setup();
    do {
        double const power = pulsarPower(settings.recipe, simulation_.time());
        setup.source[0][1] = static_cast<float>(power);
        setup.source[0][2] = static_cast<float>(2.0 * power / (kWindSpeed * kWindSpeed));
        if (!run(target, 0.5 * kWindSpeed, wait ? kWaitBatch : steps, wait, changed, error)) {
            return false;
        }
    } while (wait && simulation_.time() < target);
    if (changed || !(remnant_shaded_ == settings)) {
        remnantShade(setup, settings);
        simulation_.shade();
        remnant_shaded_ = settings;
        changed = true;
    }
    status_.time = simulation_.time() * kRemnantTimeUnitYears;
    status_.time_step = simulation_.timeStep() * kRemnantTimeUnitYears;
    status_.steps = simulation_.steps();
    status_.resolution = simulation_.resolution();
    status_.mass = simulation_.totals().mass;
    status_.source_on = true;
    return true;
}

} // namespace helios::examples

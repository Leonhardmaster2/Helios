// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// The two gas-dynamical objects of the deep-sky lab.  Both are runs of the
// same solver (hydro_simulation.hpp) from initial conditions that contain
// none of the structure seen at the end.
//
// Emission nebula.  A round molecular cloud with a turbulent velocity field
// and nothing else.  The turbulence shocks the gas into sheets and filaments.
// Then a massive star switches on: its ionising photons heat the gas they
// reach to 10^4 K, the hot bubble pushes into the cloud, and dense clumps
// that shadow the gas behind them are left standing as pillars with bright
// rims.  Colours follow from which ions the radiation field can maintain.
//
// Supernova remnant.  The freely expanding ejecta of a supernova, a few
// centuries old, ploughing into the gas around the star.  A blast wave runs
// ahead; the ejecta are slowed by the much lighter gas they have swept up,
// and a dense fluid decelerated by a light one is Rayleigh-Taylor unstable:
// the boundary between them breaks into fingers and mushrooms.  At the
// centre the wind of the pulsar the explosion left behind blows a bubble
// that shines by synchrotron radiation.
//
// Approximations are listed with the passes in shaders/hydro_*.slang; the
// largest are no magnetic fields (so no pulsar jet or torus), no gravity of
// the gas itself, and temperatures set by fast heating and cooling instead
// of evolved.
#pragma once

#include "hydro_simulation.hpp"

#include "helios/math/vec.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace helios::examples {

// Parsec, km/s, hydrogen atoms per cm^3.
struct NebulaRecipe {
    std::uint32_t seed = 7;
    std::uint32_t resolution = 256; // cells per edge
    // The box, and the stretching of its grid: the central cells are a /
    // sinh(a) of the mean, the edge cells cosh(a) times the central ones.
    double half_extent_pc = 14.0;
    double stretch = 2.0;
    double cloud_density = 300.0;
    double cloud_radius_pc = 5.0;
    // The medium around the cloud: the warm neutral medium of a galaxy's
    // disk, a few tenths of an atom per cm^3.
    double ambient_density = 0.5;
    // Root-mean-square turbulent speed (three-dimensional).
    double turbulent_speed_km_s = 3.0;
    // The star switches on once the turbulence has had this long to act.
    double ignition_myr = 1.0;
    // log10 of the ionising photons per second: 49.5 is an O5 star.
    double ionising_rate_log10 = 49.5;
    // Mass of the ionising star (it sets the visible light the dust
    // scatters), and where it sits: this fraction of the cloud radius from
    // the centre along +x (0 puts it near the centre).
    double massive_star_solar = 40.0;
    double star_offset = 0.0;
    std::uint32_t cluster_stars = 60;

    [[nodiscard]] bool operator==(NebulaRecipe const&) const = default;
};

struct NebulaSettings {
    NebulaRecipe recipe{};
    // Simulation time to advance to.
    double time_myr = 1.3;
    float emission = 1.0f;
    float dust_opacity = 1.0f; // 1 = the Milky Way's dust-to-gas ratio
    float star_brightness = 1.0f;

    [[nodiscard]] bool operator==(NebulaSettings const&) const = default;
};

// Parsec, 1000 km/s, solar masses.
struct RemnantRecipe {
    std::uint32_t seed = 3;
    std::uint32_t resolution = 256;
    double half_extent_pc = 4.0;
    double stretch = 1.0;
    double explosion_energy_1e51_erg = 1.0;
    double ejecta_mass_solar = 4.0;
    // Hydrogen atoms per cm^3 of the gas around the star.
    double ambient_density = 2.0;
    // Spin-down power of the pulsar at birth, its spin-down time and braking
    // index: L(t) = L0 (1 + t / tau)^(-(n + 1) / (n - 1)).
    double pulsar_power_1e38_erg_s = 35.0;
    double spin_down_years = 700.0;
    double braking_index = 2.5;
    double start_years = 30.0;
    // Fractional rms of the density fluctuations in the ejecta.
    double clumpiness = 0.15;
    // Power-law index of the ejecta envelope (Chevalier 1982): about 7 for
    // a white dwarf's explosion, 9 to 12 for a core collapse.
    double envelope_index = 9.0;
    // Whether swept-up gas that has cooled below about a million kelvin
    // radiates its heat away too (the radiative phase of an old remnant);
    // a young remnant's shocked gas is too hot and thin to.
    bool radiative = false;

    [[nodiscard]] bool operator==(RemnantRecipe const&) const = default;
};

struct RemnantSettings {
    RemnantRecipe recipe{};
    double time_years = 560.0;
    float filaments = 1.0f;
    float synchrotron = 1.0f;
    float pulsar = 1.0f;
    float dust = 1.0f;

    [[nodiscard]] bool operator==(RemnantSettings const&) const = default;
};

struct GasStatus {
    double time = 0.0;       // Myr (nebula) or years (remnant)
    double time_step = 0.0;  // same unit
    std::uint64_t steps = 0;
    std::uint32_t resolution = 0;
    double mass = 0.0;       // inside the box; solar masses
    double setup_seconds = 0.0;
    double last_advance_ms = 0.0; // wall time of the most recent batch of steps
    std::uint32_t last_advance_steps = 0;
    bool source_on = false;  // the star (nebula) has switched on
};

// A star drawn over the gas.
struct GasPointSource {
    math::Vec3d position{};
    double temperature_k = 0.0;
    double luminosity = 0.0; // photopic, solar units
};

class GasScene {
  public:
    [[nodiscard]] bool initialize(Diligent::IRenderDevice* device,
                                  Diligent::IDeviceContext* context, std::string& error);

    // Starts (or restarts) the run when the recipe changed or the target time
    // moved backwards, takes up to `steps` time steps towards the target (all
    // of them with `wait`), and refreshes the emission volume.  `changed`
    // reports whether the emission volume differs from before.
    [[nodiscard]] bool update(NebulaSettings const& settings, std::uint32_t threads,
                              std::uint32_t steps, bool wait, bool& changed, std::string& error);
    [[nodiscard]] bool update(RemnantSettings const& settings, std::uint32_t threads,
                              std::uint32_t steps, bool wait, bool& changed, std::string& error);

    [[nodiscard]] Diligent::ITexture* emission() const noexcept { return simulation_.emission(); }
    [[nodiscard]] GasStatus const& status() const noexcept { return status_; }
    // Half-width of the simulated box, in parsec, and the stretching of its
    // grid (lib/hydro.slang).
    [[nodiscard]] double halfExtent() const noexcept { return simulation_.setup().half_extent; }
    [[nodiscard]] double stretch() const noexcept { return simulation_.setup().stretch; }
    // Visual optical depth per unit of the emission volume's dust channel per
    // parsec, with the Milky Way's dust-to-gas ratio.
    [[nodiscard]] double dustOpacity() const noexcept;
    // Stars to draw; empty until they exist.
    [[nodiscard]] std::vector<GasPointSource> const& sources() const noexcept { return visible_; }
    // Light the medium outside the box scatters per unit length at one
    // parsec from the ionising star (zero without one), and that star's
    // position: the display continues the medium beyond the box with them.
    [[nodiscard]] math::Vec3d ambientScatter() const noexcept;
    [[nodiscard]] math::Vec3d starPosition() const noexcept;
    [[nodiscard]] HydroSimulation& simulation() noexcept { return simulation_; }

  private:
    [[nodiscard]] bool run(double target, double signal_floor, std::uint32_t steps, bool synchronous,
                           bool& changed, std::string& error);

    HydroSimulation simulation_{};
    NebulaRecipe nebula_{};
    RemnantRecipe remnant_{};
    NebulaSettings nebula_shaded_{};
    RemnantSettings remnant_shaded_{};
    bool is_remnant_ = false;
    std::vector<GasPointSource> cluster_;
    std::vector<GasPointSource> visible_;
    GasStatus status_{};
};

// Conversion of the solver's time units.
inline constexpr double kNebulaTimeUnitMyr = 0.977792;   // pc / (km/s)
inline constexpr double kRemnantTimeUnitYears = 977.792; // pc / (1000 km/s)

} // namespace helios::examples

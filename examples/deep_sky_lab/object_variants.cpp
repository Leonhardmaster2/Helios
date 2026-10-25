// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "object_variants.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace helios::examples {
namespace {

// ---- emission nebula ----------------------------------------------------------
//
// All four are the same solver (a turbulent cloud, then a star switches on);
// they differ in how dense and turbulent the cloud is, where the star sits
// and how hard its radiation is.
constexpr std::array<ObjectVariantInfo, 4> kNebulaVariants{{
    {"orion", "H II region (Orion-like)",
     "One O star (40 solar masses, 3e49 ionising photons per second) inside a cloud of 300 "
     "atoms per cm^3: a blister of ionised gas with bright rims where it eats into the dense "
     "filaments the turbulence made, [O III] teal near the star and hydrogen red at the "
     "edges, blue starlight scattered by dust.",
     {19.0, 12.0, -64.0, 40.0}, 1.3, 0.0f},
    {"pillars", "Pillars (Eagle-like)",
     "A denser, more turbulent cloud (900 per cm^3, 5 km/s) given longer to fragment before "
     "a hotter cluster (6e49 photons per second) switches on. Dense clumps shadow the gas "
     "behind them, and the ionisation front overtakes everything else: the clumps are left as "
     "pillars pointing at the stars, with bright ionised tips (Gritschneder et al. 2010; "
     "Mackey & Lim 2010).",
     {25.0, 10.0, -64.0, 40.0}, 1.8, 0.0f},
    {"blister", "Blister and champagne flow",
     "The star sits near the edge of its cloud. The ionised gas breaks out of the cloud on "
     "that side and streams into the thin gas beyond at its own sound speed, a champagne "
     "flow (Tenorio-Tagle 1979; Whitworth 1979); inside, the front keeps eating into the "
     "cloud. Most H II regions are of this kind.",
     {24.0, 10.0, -30.0, 40.0}, 1.6, 0.0f},
    {"reflection", "Reflection nebula (B star)",
     "A 12 solar-mass B star near the edge of a quiet cloud (200 per cm^3, 2 km/s), seen from "
     "the lit side. It is too cool to ionise more than a pocket of gas (the faint ring), so "
     "what is seen is its own light scattered by the dust of the cloud, blue because small "
     "grains scatter blue light best (the Pleiades, NGC 7023). The filaments are the "
     "turbulence's; nothing else shapes the cloud. Tens of times fainter than an H II region: "
     "the exposure is raised to show it, as a long photographic exposure would.",
     {12.0, 10.0, -30.0, 40.0}, 1.4, 4.0f},
}};

// ---- supernova remnant --------------------------------------------------------
constexpr std::array<ObjectVariantInfo, 4> kRemnantVariants{{
    {"core-collapse", "Core-collapse remnant with pulsar (young)",
     "Ejecta of 4 solar masses and 1e51 erg, a few centuries old, ploughing into gas of 2 "
     "atoms per cm^3, with the pulsar left by the explosion blowing a bubble at the centre. "
     "The blast wave runs ahead; behind it the swept-up gas decelerates the ejecta, whose "
     "boundary breaks into Rayleigh-Taylor fingers (Chevalier, Blondin & Emmering 1992).",
     {8.6, 18.0, -90.0, 40.0}, 560.0, 0.0f},
    {"type-ia", "Type Ia in thin gas (Tycho-like)",
     "A white dwarf's explosion: 1.4 solar masses, 1.2e51 erg, a shallower envelope (n = 7, "
     "Chevalier 1982) and no pulsar, in gas of 0.3 per cm^3. The remnant is a bare shell: a "
     "Balmer-dominated blast wave outside, the fingers of shocked ejecta inside, and nothing "
     "at the centre (Tycho's remnant at 450 years; Warren et al. 2005). In visible light "
     "such a remnant is only the faint Balmer rim: the exposure is raised to show it.",
     {12.0, 18.0, -90.0, 40.0}, 450.0, 2.0f},
    {"crab", "Pulsar-wind nebula dominated (Crab-like)",
     "A weak explosion (1e50 erg, 5 solar masses; Yang & Chevalier 2015) in thin gas, but "
     "an energetic pulsar (3.5e39 erg/s at birth, spinning down over 700 years). Its wind "
     "bubble sweeps the slow ejecta into a shell and lights it from inside; the shell is "
     "Rayleigh-Taylor unstable and its fingers point inwards (Hester et al. 1996; Porth et "
     "al. 2014). No magnetic field, so no torus or jet.",
     {12.0, 18.0, -90.0, 40.0}, 970.0, 0.0f},
    {"radiative", "Old remnant in dense gas, radiative shell (Cygnus Loop-like)",
     "Twelve thousand years after a 3e50 erg explosion in gas of 10 atoms per cm^3, no pulsar. "
     "The blast wave has slowed below 200 km/s, where the gas it shocks radiates its heat "
     "away in less than the remnant's age and collapses into a thin, dense shell (the "
     "radiative phase; Cioffi, McKee & Bertschinger 1988, Blondin et al. 1998); the shell "
     "is what shines, in the lines of recombining gas. Its thickness is below a cell here, "
     "so its structure is coarser than the real filaments.",
     {24.0, 18.0, -90.0, 40.0}, 12000.0, 0.0f},
}};

// ---- disk galaxy ---------------------------------------------------------------
constexpr std::array<ObjectVariantInfo, 4> kGalaxyVariants{{
    {"milky-way", "Milky-Way-like (Sb)",
     "A 4.5e10 solar-mass stellar disk with 8e9 of gas and a 6e9 bulge in a halo that holds "
     "the rotation curve flat at 180 km/s. Swing-amplified spiral arms grow from the noise "
     "of the particles; a bar forms after a few hundred Myr; star formation follows the gas "
     "into the arms. At the centre a black hole of 4.3e6 solar masses (Sagittarius A*).",
     {46.0, 38.0, -70.0, 40.0}, 13000.0, 0.0f},
    {"barred", "Barred, disk-dominated (SBb)",
     "The same disk in a lighter halo (150 km/s) with a smaller bulge, less gas and a colder "
     "disk (Q = 1.25). A disk whose own gravity dominates is bar-unstable (Ostriker & Peebles 1973; "
     "Efstathiou, Lake & Negroponte 1982): a strong bar forms early and drives gas to the "
     "centre and into a ring at its ends. Central black hole 1.1e7 solar masses.",
     {46.0, 38.0, -70.0, 40.0}, 13000.0, 0.0f},
    {"flocculent", "Gas-rich flocculent (Sc)",
     "A lighter stellar disk (3e10) carrying 1.8e10 solar masses of gas and almost no bulge, "
     "in a 200 km/s halo, with a hotter stellar disk (Q = 1.5). The gas, not the stars, is "
     "the unstable component: it fragments into many short arm pieces and star-forming "
     "knots rather than two long arms (Elmegreen & Elmegreen 1987). Central black hole "
     "3.6e6 solar masses.",
     {46.0, 38.0, -70.0, 40.0}, 13000.0, 0.0f},
    {"early-type", "Early-type, bulge-dominated (Sa)",
     "A 2.5e10 solar-mass bulge, a hot disk (Q = 1.8) and little gas (2.5e9). The massive "
     "bulge raises the inner rotation curve and the hot disk resists amplification: arms "
     "stay smooth, tightly wound and faint, and little gas is left to form stars, so the "
     "light is the yellow of old populations. Central black hole 9.7e7 solar masses.",
     {46.0, 38.0, -70.0, 40.0}, 13000.0, 0.0f},
}};

// ---- black hole ---------------------------------------------------------------
constexpr std::array<ObjectVariantInfo, 4> kBlackHoleVariants{{
    {"cool-disk", "Cool disk (visualisation)",
     "The thin disk's temperature profile scaled down to peak at 5200 K, so that its radial "
     "colour structure and the Doppler shift of its two sides show in visible light. Real "
     "disks are far hotter (the next two states) and look uniformly blue-white to the eye.",
     {75.0, 6.0, -90.0, 38.0}, 0.0, 0.0f},
    {"stellar", "Stellar-mass X-ray binary (10 solar masses)",
     "A ten solar-mass black hole accreting at a tenth of the Eddington rate: the Page-Thorne "
     "disk peaks at 5 million K and radiates in X-rays. In visible light the disk is on the "
     "Rayleigh-Jeans tail everywhere, so it is blue-white with only a mild brightening "
     "inwards; the brightness asymmetry is Doppler beaming of the approaching side.",
     {75.0, 6.0, -90.0, 38.0}, 0.0, -1.5f},
    {"supermassive", "Supermassive, quasar disk (1e8 solar masses)",
     "A hundred-million solar-mass black hole at a tenth of Eddington: the disk's peak "
     "temperature scales as the inverse fourth root of the mass, 90 000 K, so it peaks in "
     "the ultraviolet (the big blue bump of quasars; Shields 1978). The size of the hole in "
     "the picture is the same: everything here is in units of the gravitational radius.",
     {75.0, 6.0, -90.0, 38.0}, 0.0, -1.0f},
    {"bare", "Bare black hole (no disk)",
     "Light bending alone: the shadow, the photon ring, and the sky lensed around it, with "
     "each star brightened by the magnification and the Milky Way smeared into arcs.",
     {75.0, 6.0, -90.0, 38.0}, 0.0, 0.0f},
}};

} // namespace

std::span<ObjectVariantInfo const> nebulaVariants() noexcept {
    return kNebulaVariants;
}
std::span<ObjectVariantInfo const> remnantVariants() noexcept {
    return kRemnantVariants;
}
std::span<ObjectVariantInfo const> galaxyVariants() noexcept {
    return kGalaxyVariants;
}
std::span<ObjectVariantInfo const> blackHoleVariants() noexcept {
    return kBlackHoleVariants;
}

NebulaRecipe nebulaVariantRecipe(std::size_t index, NebulaRecipe base) noexcept {
    NebulaRecipe r;
    r.seed = base.seed;
    r.resolution = base.resolution;
    switch (std::min(index, kNebulaVariants.size() - 1)) {
    case 0:
        break; // the defaults
    case 1:
        r.half_extent_pc = 16.0;
        r.cloud_density = 900.0;
        r.turbulent_speed_km_s = 5.0;
        r.ignition_myr = 1.6;
        r.ionising_rate_log10 = 49.8;
        r.massive_star_solar = 55.0;
        break;
    case 2:
        r.half_extent_pc = 16.0;
        r.cloud_radius_pc = 4.5;
        r.star_offset = 0.8;
        r.ignition_myr = 0.9;
        break;
    case 3:
        r.half_extent_pc = 12.0;
        r.cloud_density = 200.0;
        r.turbulent_speed_km_s = 2.0;
        r.ignition_myr = 0.6;
        r.star_offset = 0.6;
        // A B1 star of 12 solar masses: about 1e46 ionising photons per
        // second (Sternberg, Hoffmann & Pauldrach 2003), enough for a
        // pocket of ionised gas a parsec across.
        r.ionising_rate_log10 = 46.0;
        r.massive_star_solar = 12.0;
        r.cluster_stars = 40;
        break;
    }
    return r;
}

RemnantRecipe remnantVariantRecipe(std::size_t index, RemnantRecipe base) noexcept {
    RemnantRecipe r;
    r.seed = base.seed;
    r.resolution = base.resolution;
    switch (std::min(index, kRemnantVariants.size() - 1)) {
    case 0:
        break; // the defaults
    case 1:
        r.half_extent_pc = 6.5;
        r.explosion_energy_1e51_erg = 1.2;
        r.ejecta_mass_solar = 1.4;
        r.ambient_density = 0.3;
        r.pulsar_power_1e38_erg_s = 0.0;
        r.envelope_index = 7.0;
        break;
    case 2:
        // Slow ejecta: the run starts at 150 years, when their core spans
        // about nine of the central cells, with the wind's energy to then
        // already in its bubble.
        r.half_extent_pc = 8.0;
        r.stretch = 2.5;
        r.explosion_energy_1e51_erg = 0.1;
        r.ejecta_mass_solar = 5.0;
        r.ambient_density = 0.3;
        r.pulsar_power_1e38_erg_s = 35.0;
        r.start_years = 150.0;
        break;
    case 3:
        // Started as late as free expansion allows (the swept-up mass is a
        // quarter of the ejecta mass at 120 years), so that the ejecta span
        // as many cells as possible: their layout on the grid imprints
        // itself on the shell otherwise.
        r.half_extent_pc = 10.0;
        r.stretch = 1.2;
        r.explosion_energy_1e51_erg = 0.3;
        r.ejecta_mass_solar = 4.0;
        r.ambient_density = 10.0;
        r.pulsar_power_1e38_erg_s = 0.0;
        r.start_years = 120.0;
        r.radiative = true;
        break;
    }
    return r;
}

GalaxyRecipe galaxyVariantRecipe(std::size_t index, GalaxyRecipe base) noexcept {
    GalaxyRecipe r = base; // seed, grid and particle counts are the caller's
    GalaxyRecipe const defaults;
    r.disk_mass = defaults.disk_mass;
    r.gas_mass = defaults.gas_mass;
    r.bulge_mass = defaults.bulge_mass;
    r.halo_speed_km_s = defaults.halo_speed_km_s;
    r.toomre_q = defaults.toomre_q;
    // Central black holes: Sagittarius A* for the Milky Way (it lies below
    // the relation, as black holes in pseudo-bulges do); for the others the
    // black hole-bulge mass relation of Kormendy & Ho (2013),
    // M_BH = 4.9e8 (M_bulge / 1e11)^1.17 solar masses.
    r.black_hole_mass = defaults.black_hole_mass;
    switch (std::min(index, kGalaxyVariants.size() - 1)) {
    case 0:
        break;
    case 1:
        r.halo_speed_km_s = 150.0;
        r.bulge_mass = 0.4;
        r.gas_mass = 0.6;
        r.toomre_q = 1.25;
        r.black_hole_mass = 1.1e-3;
        break;
    case 2:
        r.disk_mass = 3.0;
        r.gas_mass = 1.8;
        r.bulge_mass = 0.15;
        r.halo_speed_km_s = 200.0;
        r.toomre_q = 1.5;
        r.black_hole_mass = 3.6e-4;
        break;
    case 3:
        r.disk_mass = 4.0;
        r.gas_mass = 0.25;
        r.bulge_mass = 2.5;
        r.halo_speed_km_s = 220.0;
        r.toomre_q = 1.8;
        r.black_hole_mass = 9.7e-3;
        break;
    }
    return r;
}

double blackHoleDiskPeakTemperatureK(double mass_solar, double eddington_ratio) noexcept {
    constexpr double kGravity = 6.674e-8;
    constexpr double kLightSpeed = 2.99792458e10;
    constexpr double kStefanBoltzmann = 5.6704e-5;
    constexpr double kSolarMass = 1.989e33;
    constexpr double kEddingtonPerSolarMass = 1.26e38; // erg/s
    constexpr double kEfficiency = 0.057;               // of a Schwarzschild disk
    // The Shakura-Sunyaev profile T^4 = 3 G M Mdot / (8 pi sigma r^3)
    // (1 - sqrt(r_in / r)) with r_in = 6 M peaks at r = 49/6 M, where the
    // bracket times (M/r)^3 is 2.63e-4 (the Page-Thorne profile's peak is
    // within a few per cent of this).
    constexpr double kPeakFactor = 2.63e-4;
    double const mass = std::max(mass_solar, 1.0) * kSolarMass;
    double const rate = std::max(eddington_ratio, 1.0e-6) * kEddingtonPerSolarMass * mass_solar /
                        (kEfficiency * kLightSpeed * kLightSpeed);
    double const gravitational_radius = kGravity * mass / (kLightSpeed * kLightSpeed);
    double const t4 = 3.0 * kGravity * mass * rate * kPeakFactor /
                      (8.0 * std::numbers::pi * kStefanBoltzmann * gravitational_radius *
                       gravitational_radius * gravitational_radius);
    return std::pow(t4, 0.25);
}

void applyBlackHoleVariant(std::size_t index, BlackHoleSettings& settings) noexcept {
    BlackHoleSettings const defaults;
    settings.disk_enabled = true;
    settings.disk_outer_radius_in_masses = defaults.disk_outer_radius_in_masses;
    settings.disk_peak_temperature_k = defaults.disk_peak_temperature_k;
    settings.disk_opacity = defaults.disk_opacity;
    switch (std::min(index, kBlackHoleVariants.size() - 1)) {
    case 0:
        break;
    case 1:
        settings.disk_peak_temperature_k =
            static_cast<float>(blackHoleDiskPeakTemperatureK(10.0, 0.1));
        settings.disk_outer_radius_in_masses = 120.0f;
        break;
    case 2:
        settings.disk_peak_temperature_k =
            static_cast<float>(blackHoleDiskPeakTemperatureK(1.0e8, 0.1));
        settings.disk_outer_radius_in_masses = 120.0f;
        break;
    case 3:
        settings.disk_enabled = false;
        break;
    }
}

// ---- planet --------------------------------------------------------------------------
// Views: distance is altitude above ground (m), elevation latitude and
// azimuth longitude.

namespace {

constexpr std::array<ObjectVariantInfo, kPlanetSystemKindCount> kPlanetVariants{{
    {"earth-like", "Earth-like world",
     "A world of Earth's size and density: 30 % land in domain-warped continents with ridged "
     "mountain belts and oceans to the sea level, an N2/O2 atmosphere (Rayleigh coefficients of "
     "Bucholtz 1995) with a little aerosol, water clouds between 1.5 and 6 km covering half the "
     "sky, a 23.9 h day and one Moon-sized moon sixty radii out.",
     {13'629'000.0, 18.8, -20.0, 60.0}, 0.0, 0.0f},
    {"desert-world", "Desert world",
     "A Mars-like world: half Earth's radius, no ocean, a thin atmosphere whose dust outweighs its "
     "molecules, cratered highlands, thin high water-ice clouds and two small captured moons close "
     "in, on the orbits of Phobos and Deimos.",
     {7'000'000.0, 12.0, -30.0, 60.0}, 0.0, 0.0f},
    {"ice-world", "Ice world",
     "A frozen ocean world: a bright ice crust with cracks and craters, a thin atmosphere, a "
     "five-day rotation and one icy moon twelve radii out. Its temperature keeps snow and ice down "
     "to the equator.",
     {5'000'000.0, 25.0, -40.0, 60.0}, 0.0, -0.5f},
    {"ringed-gas-giant", "Ringed gas giant",
     "A Saturn-like giant: no surface, banded cloud decks with storms, a 10.6 h day, rings from "
     "1.24 to 2.27 radii (the C to the A ring) of normal optical depth up to 1.8, with gaps opened "
     "at the inner Lindblad resonances of its moons (Goldreich & Tremaine 1978, 1980) and along "
     "the orbit of any moon inside them, and four icy moons, the last a large hazy one far out.",
     {230'000'000.0, 24.0, -30.0, 50.0}, 0.0, 0.0f},
}};

} // namespace

std::span<ObjectVariantInfo const> planetVariants() noexcept { return kPlanetVariants; }

PlanetSystem planetVariantSystem(std::size_t index, std::uint32_t seed) noexcept {
    return planetSystemPreset(static_cast<PlanetSystemKind>(std::min(index, kPlanetSystemKindCount - 1)),
                              seed);
}

} // namespace helios::examples

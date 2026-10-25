// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Every star of the galaxy, and the planets and moons around it.
//
// A star particle of the N-body run stands for all the stars born together
// in it: N = (particle mass) / (mean stellar mass) stars of one age, from the
// Kroupa (2001) mass function that also gives the particle its light
// (stellar_population.hpp).  Here each of them is named and worked out:
//
//   rank      star r of a particle is its r-th most massive: its initial
//             mass is where the mass function's upper tail holds r + u of
//             the N stars (u in [0, 1) from a hash), so the N stars are the
//             mass function itself, without sampling noise;
//   state     main sequence, giant, white dwarf, neutron star or black hole,
//             from the initial mass and the particle's age, with the
//             population's mass-luminosity relations and lifetimes, and the
//             energy radiated after the main sequence from the
//             fuel-consumption theorem (Renzini & Buzzoni 1986), spread over
//             the giant branch with its observed luminosity function;
//   place     an offset from the particle's centre, Gaussian over the
//             distance across which its stars are spread (the width of the
//             particle's image in the galaxy pass);
//   planets   three or four, grown by core accretion from a disk whose
//             solids scale with the star's mass and metallicity; moons where
//             the planet's Hill sphere leaves room outside its Roche limit.
//
// Everything is a pure function of (particle, rank) and the particle's age and
// birth time, so a star found in the image is found again with the same
// system.  shaders/galaxy_resolved_stars.slang evaluates the same functions,
// in the same order, for the stars it draws.
//
// Units: solar masses, luminosities and radii for stars; Earth masses and
// radii, astronomical units and days for planets; kilometres for moons.
#pragma once

#include "helios/math/vec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace helios::examples {

// ---- the mass function -------------------------------------------------------

inline constexpr double kImfMinimumMass = 0.08;
inline constexpr double kImfMaximumMass = 120.0;

// Mean initial mass of a star (solar masses).
[[nodiscard]] double imfMeanMass() noexcept;
// Stars born in a particle of this mass.
[[nodiscard]] std::uint32_t starsInParticle(double particle_mass_solar) noexcept;
// Fraction of stars born more massive than `mass`, and its inverse.
[[nodiscard]] double imfTailFraction(double mass) noexcept;
[[nodiscard]] double imfTailMass(double fraction) noexcept;

// ---- single stars ------------------------------------------------------------

enum class StarPhase : std::uint8_t { MainSequence, Giant, WhiteDwarf, NeutronStar, BlackHole };

struct StarState {
    StarPhase phase = StarPhase::MainSequence;
    double initial_mass = 0.0; // solar masses
    double mass = 0.0;         // now
    double luminosity = 0.0;   // bolometric, solar
    double radius = 0.0;       // solar radii
    double temperature_k = 0.0;
    double age_myr = 0.0;
    double main_sequence_myr = 0.0; // lifetime on the main sequence
    double giant_myr = 0.0;         // after it, until the remnant forms
    double largest_radius = 0.0;    // largest radius reached so far (engulfs planets)
};

// Main-sequence lifetime and the time it spends as a giant (Myr).
[[nodiscard]] double mainSequenceMyr(double initial_mass) noexcept;
[[nodiscard]] double giantMyr(double initial_mass) noexcept;
[[nodiscard]] StarState evolveStar(double initial_mass, double age_myr) noexcept;

// Initial masses that have left the main sequence, and that have become
// remnants, by an age (capped at the top of the mass function).
[[nodiscard]] double turnoffMassAt(double age_myr) noexcept;
[[nodiscard]] double deadMassAt(double age_myr) noexcept;

// ---- stars of a particle -------------------------------------------------------

// The hash shared with the shaders: 32-bit PCG output function of a key.
[[nodiscard]] std::uint32_t starHash(std::uint32_t particle, std::uint32_t rank, std::uint32_t stream) noexcept;
[[nodiscard]] float starUnit(std::uint32_t particle, std::uint32_t rank, std::uint32_t stream) noexcept;

struct StarKey {
    std::uint32_t particle = 0;
    std::uint32_t rank = 0;
    [[nodiscard]] bool operator==(StarKey const&) const = default;
};

// What a particle's stars share.
struct ParticleStars {
    std::uint32_t particle = 0;
    std::uint32_t count = 0;   // stars born
    std::uint32_t dead = 0;    // ranks [0, dead) are remnants
    std::uint32_t giants = 0;  // ranks [dead, dead + giants) are giants
    double age_myr = 0.0;
    double birth_myr = 0.0;    // in the run's clock
    bool bulge = false;
    double spread_pc = 0.0;    // Gaussian width of the stars in the plane
    double thickness_pc = 0.0; // and perpendicular to it
};

// The stars of one particle at an age.
[[nodiscard]] ParticleStars particleStars(std::uint32_t particle, double particle_mass_solar, double age_myr,
                                          double birth_myr, bool bulge, double spread_pc,
                                          double thickness_pc) noexcept;

// Rank of the star in `slot`: slots run over the living stars from the
// brightest (giants, then the main sequence downwards) and then the
// remnants.
[[nodiscard]] std::uint32_t rankOfSlot(ParticleStars const& stars, std::uint32_t slot) noexcept;
[[nodiscard]] std::uint32_t slotOfRank(ParticleStars const& stars, std::uint32_t rank) noexcept;
[[nodiscard]] double initialMassOfRank(ParticleStars const& stars, std::uint32_t rank) noexcept;
// Offset of the star from its particle's centre (pc).
[[nodiscard]] math::Vec3d starOffsetPc(ParticleStars const& stars, std::uint32_t rank) noexcept;
// [Fe/H] of a star: from the age-metallicity relation of the disk, or the
// bulge's distribution, with a scatter.
[[nodiscard]] double starMetallicity(ParticleStars const& stars, std::uint32_t rank) noexcept;

// Living stars brighter than `luminosity` (bolometric, solar), counted from
// slot 0: all giants and the main sequence down to that luminosity.
[[nodiscard]] std::uint32_t slotsBrighterThan(ParticleStars const& stars, double luminosity) noexcept;

// Fraction of a population's light (bolometric) in its first `slots` slots.
// Built once from evolveStar over a grid of ages and slot fractions.
class ResolvedLightTable {
  public:
    ResolvedLightTable();
    [[nodiscard]] double fraction(double age_myr, double slot_fraction) const noexcept;

  private:
    std::vector<float> values_; // ages x fractions
};

// ---- planets and moons ---------------------------------------------------------

enum class PlanetKind : std::uint8_t {
    Rocky,      // terrestrial, airless or with a thin atmosphere
    Temperate,  // terrestrial at the temperature of liquid water
    Cloudy,     // terrestrial under thick clouds (Venus-like)
    Lava,       // dayside molten
    Icy,        // terrestrial with an ice surface
    SubNeptune, // a few Earth masses with a hydrogen envelope
    IceGiant,
    GasGiant,
};
[[nodiscard]] char const* planetKindName(PlanetKind kind) noexcept;

struct Moon {
    double mass_earth = 0.0;
    double radius_km = 0.0;
    double orbit_km = 0.0;
    double period_days = 0.0;
    double phase_rad = 0.0; // orbital angle at time zero
    bool icy = false;
    math::Vec3d albedo{}; // linear sRGB
};

inline constexpr std::size_t kMaxPlanets = 4;
inline constexpr std::size_t kMaxMoons = 4;

struct Planet {
    PlanetKind kind = PlanetKind::Rocky;
    double mass_earth = 0.0;
    double radius_earth = 0.0;
    double core_earth = 0.0;     // solids accreted
    double orbit_au = 0.0;       // semi-major axis
    double eccentricity = 0.0;
    double inclination_rad = 0.0;
    double node_rad = 0.0;
    double periapsis_rad = 0.0;
    double phase_rad = 0.0;      // mean anomaly at time zero
    double period_days = 0.0;
    double equilibrium_k = 0.0;  // at Bond albedo `bond_albedo`, full redistribution
    double bond_albedo = 0.0;
    math::Vec3d albedo{};        // linear sRGB reflectance
    double day_hours = 0.0;
    bool tidally_locked = false;
    // Axial tilt, and the planet's equatorial frame in the system's reference
    // frame (x, y in the equator, z the pole): rings and regular moons lie in
    // the equator.
    double obliquity_rad = 0.0;
    std::array<math::Vec3d, 3> equator{math::Vec3d{1.0, 0.0, 0.0}, math::Vec3d{0.0, 1.0, 0.0},
                                       math::Vec3d{0.0, 0.0, 1.0}};
    bool rings = false;
    double ring_inner = 0.0;     // planet radii
    double ring_outer = 0.0;
    std::uint32_t moon_count = 0;
    std::array<Moon, kMaxMoons> moons{};
    char const* moon_note = ""; // why it has fewer moons than it could
};

struct StarSystem {
    StarKey key{};
    StarState star{};
    double metallicity = 0.0;   // [Fe/H]
    double snow_line_au = 0.0;
    double disk_solids = 0.0;   // relative to the minimum-mass extrasolar nebula
    std::uint32_t planet_count = 0;
    std::array<Planet, kMaxPlanets> planets{};
    std::uint32_t lost_planets = 0; // engulfed by the star or unbound by its supernova
    std::string name;
    // The reference plane of the orbits in galaxy coordinates: x, y in the
    // plane, z its normal (each system is oriented at random).
    std::array<math::Vec3d, 3> axes{};
};

[[nodiscard]] StarSystem generateStarSystem(ParticleStars const& stars, std::uint32_t rank);

// Positions on the Kepler orbits at a time given as a coarse and a fine part
// (seconds; their sum is the time, the split keeps hours resolved after
// billions of years): a planet in AU in the frame of the system's reference
// plane (StarSystem::axes, x along the node line), a moon in km in its
// planet's equatorial frame (Planet::equator).
[[nodiscard]] math::Vec3d planetPosition(Planet const& planet, double coarse_s, double fine_s) noexcept;
[[nodiscard]] math::Vec3d moonPosition(Moon const& moon, double coarse_s, double fine_s) noexcept;

// A short name for a star: catalogue number from its particle and rank.
[[nodiscard]] std::string starName(StarKey const& key);
[[nodiscard]] char const* starPhaseName(StarPhase phase) noexcept;
// Spectral class from the effective temperature (O B A F G K M), or the
// remnant's type.
[[nodiscard]] std::string spectralClass(StarState const& star);

} // namespace helios::examples

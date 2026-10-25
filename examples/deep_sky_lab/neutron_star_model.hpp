// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// The neutron star of the deep-sky lab: its physical parameters, the
// quantities derived from them that the shader (shaders/neutron_star.slang)
// and the UI need, and the named states it can be in.
//
// The star always has 1.4 solar masses; lengths are in M = GM/c^2
// (2.07 km) and times in M/c (6.9 microseconds).
#pragma once

#include <array>
#include <cstdint>

namespace helios::examples {

enum class NeutronStarBand : std::uint8_t {
    Visible = 0, // what an eye would see: everything hotter than 1e5 K is blue-white
    XRay = 1,    // 0.2-10 keV mapped onto the visible band: temperatures tell apart
};

struct NeutronStarSettings {
    float radius_in_masses = 5.8f; // 12 km
    float spin_hz = 30.0f;
    float magnetic_inclination_deg = 45.0f;
    float log10_field_gauss = 12.6f; // dipole field at the magnetic pole

    // Surface temperature at the magnetic poles, and the crust's heat
    // conductivity across the field relative to along it: 1 gives a uniform
    // surface, 0.01 a magnetic equator at a third of the polar temperature.
    float polar_temperature_k = 1.5e6f;
    float conductivity_ratio = 0.03f;
    // Polar caps heated by the return current of the magnetosphere (0: none).
    float cap_temperature_k = 2.5e6f;

    // Plasma streaming out on the open field lines: brightness relative to
    // the surface (free: no theory fixes the radiative efficiency) and
    // Lorentz factor (real ones are 100 and up).
    float outflow = 1.0f;
    float lorentz_factor = 2.5f;
    // The current sheet of the wind beyond the light cylinder, and the wind's
    // Lorentz factor (real ones are far higher; at these the whole sheet is
    // visible rather than only the part moving towards the camera).
    float wind_sheet = 0.35f;
    float wind_lorentz_factor = 2.0f;
    // Magnetar: glow of the twisted bundle of closed field lines reaching
    // beyond this many stellar radii (0: none).
    float twist_glow = 0.0f;
    float twist_reach_radii = 4.0f;

    // Accretion from a disk fed by a companion.  The disk reaches out to the
    // tidal truncation radius set by the binary's period and the donor's
    // mass, and its outer parts are heated mostly by the X-rays of the inner
    // parts: the fraction of the accretion luminosity they intercept is
    // about 5e-3 in observed low-mass X-ray binaries.
    bool accreting = false;
    float log10_accretion_rate = -10.0f; // solar masses per year
    float orbital_period_h = 2.01f;      // SAX J1808.4-3658
    float donor_mass_solar = 0.05f;
    float irradiation_fraction = 5.0e-3f;
    float stream_brightness = 1.0f;      // free parameter
    float stream_width = 0.2f;           // of r_m: how deep matter penetrates the field

    float grid = 0.0f; // surface graticule (diagnostic)
    NeutronStarBand band = NeutronStarBand::Visible;

    [[nodiscard]] bool operator==(NeutronStarSettings const&) const = default;
};

// Quantities that follow from the settings.
struct NeutronStarDerived {
    double omega = 0.0;              // rad per M/c
    double light_cylinder = 0.0;     // M
    double corotation_radius = 0.0;  // M (where Keplerian and spin periods agree)
    double equatorial_speed = 0.0;   // c
    double surface_redshift = 0.0;   // z
    double cap_half_angle_deg = 0.0; // of the open field lines
    double spin_down_erg_s = 0.0;    // vacuum dipole
    double magnetospheric_radius = 0.0; // M; 0 without a disk
    bool propeller = false;             // r_m beyond corotation: no accretion
    double disk_temperature_scale_k = 0.0; // T0 of the Shakura-Sunyaev profile
    double inner_disk_temperature_k = 0.0; // its maximum
    double spot_temperature_k = 0.0;
    double accretion_luminosity_erg_s = 0.0;
    double binary_separation = 0.0;     // M
    double disk_outer_radius = 0.0;     // M: the disk's tidal truncation
    double irradiation_k4_m2 = 0.0;     // T_irr^4 r^2 of the irradiated disk, K^4 M^2
    double stream_launch_speed = 0.0;   // c
    double cap_reach = 0.0;             // field lines reaching beyond this are heated
    double sheet_outer = 0.0;           // M
    double emission_outer = 0.0;        // M: nothing emits beyond this
    double hottest_k = 0.0;             // of the surface
};

[[nodiscard]] NeutronStarDerived deriveNeutronStar(NeutronStarSettings const& settings) noexcept;

// Named states.  Each is a complete set of settings; the user can edit any
// of them afterwards.
enum class NeutronStarVariant : std::uint8_t {
    IsolatedCooling = 0,
    YoungPulsar = 1,
    MillisecondPulsar = 2,
    Magnetar = 3,
    AccretingPulsar = 4,
    OrthogonalPulsar = 5,
    AlignedPulsar = 6,
};
inline constexpr std::size_t kNeutronStarVariantCount = 7;

// The rotation-powered states, in the order the pulsar entry of the object
// menu lists them.
inline constexpr std::array<NeutronStarVariant, 4> kPulsarVariants{
    NeutronStarVariant::YoungPulsar, NeutronStarVariant::MillisecondPulsar,
    NeutronStarVariant::OrthogonalPulsar, NeutronStarVariant::AlignedPulsar};

struct NeutronStarVariantInfo {
    char const* key;         // command-line name
    char const* label;       // UI name
    char const* description; // what it is, and what is assumed
    // A view that shows it: distance (M), elevation and azimuth (deg), fov.
    double distance;
    double elevation_deg;
    double azimuth_deg;
    double fov_deg;
};

[[nodiscard]] NeutronStarVariantInfo const& neutronStarVariantInfo(NeutronStarVariant variant) noexcept;
[[nodiscard]] NeutronStarSettings neutronStarVariant(NeutronStarVariant variant) noexcept;

} // namespace helios::examples

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "neutron_star_model.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace helios::examples {
namespace {

// 1.4 solar masses in cgs.
constexpr double kMassGrams = 1.4 * 1.989e33;
constexpr double kGravity = 6.674e-8;         // cm^3 g^-1 s^-2
constexpr double kLightSpeed = 2.99792458e10; // cm/s
constexpr double kStefanBoltzmann = 5.6704e-5;
constexpr double kMassCm = kGravity * kMassGrams / (kLightSpeed * kLightSpeed); // GM/c^2
constexpr double kMassSeconds = kMassCm / kLightSpeed;                          // GM/c^3
constexpr double kSolarMassPerYearGrams = 1.989e33 / 3.15576e7;
// Alfven radius coefficient: r_m = xi (mu^4 / (2 G M Mdot^2))^(1/7).
constexpr double kAlfvenFactor = 0.5;
constexpr double kRadiansPerDegree = std::numbers::pi / 180.0;

constexpr std::array<NeutronStarVariantInfo, kNeutronStarVariantCount> kVariants{{
    {"isolated", "Isolated cooling star",
     "A neutron star doing nothing but cooling (like RX J1856.5-3754): a bare black-body "
     "surface, hottest at the magnetic poles because the crust conducts heat along the field "
     "far better than across it. Light bending shows more than half of it. In visible light it "
     "is a featureless blue-white ball; the X-ray band shows the temperature pattern.",
     60.0, 12.0, -90.0, 36.0},
    {"young-pulsar", "Young pulsar (Crab-like)",
     "Rotation-powered pulsar, 30 turns per second, 4e12 G. Plasma leaves on the open field "
     "lines and radiates along its motion, so the beam is bright only while it points at you: "
     "the pulse. Beyond the light cylinder the field winds into a spiral and the current sheet "
     "between the two magnetic hemispheres dissipates the wind (the striped wind; visible far "
     "out). Zoom out to see it.",
     95.0, 45.0, -90.0, 36.0},
    {"millisecond-pulsar", "Millisecond pulsar",
     "An old pulsar spun up by past accretion: 205 turns per second, a weak field (2e8 G), a "
     "cold surface with hot polar caps (what NICER maps), a light cylinder only 19 stellar radii "
     "out, so the wind's current sheet starts close to the star. The approaching limb is "
     "Doppler-brightened.",
     420.0, 30.0, -90.0, 36.0},
    {"magnetar", "Magnetar",
     "A slowly turning star with a 1e15 G field: a hot, strongly non-uniform surface, and a "
     "twisted magnetosphere whose currents flow along the closed field lines near the magnetic "
     "axis, glow, and heat their footprints. No beams: the light cylinder is far away.",
     60.0, 18.0, -90.0, 36.0},
    {"accreting", "Accreting millisecond pulsar",
     "A pulsar in a binary feeding from a disk (like SAX J1808.4-3658). The disk is cut off "
     "where the field's magnetic pressure stops it; from there matter follows the field lines "
     "to the poles in two streams and lands in crescent-shaped hot spots, whose shape and "
     "temperature follow from the field geometry and the accretion rate. The disk's far side "
     "is lifted into view by light bending. The disk reaches its tidal edge, thirty thousand "
     "stellar radii out, where the X-rays of its inner parts keep it at 10^4 K; in visible light "
     "it outshines the star. Zoom in for the streams and the spots, or switch to X-rays.",
     3000.0, 32.0, -90.0, 36.0},
    {"orthogonal", "Orthogonal rotator (interpulse)",
     "A young pulsar whose magnetic axis lies almost in its equator, 20 turns per second, "
     "3e12 G. Both beams sweep past an observer near the equator once per turn, half a turn "
     "apart: a main pulse and an interpulse (PSR B1055-52, and nearly the Crab). The wind's "
     "current sheet reaches up to both poles, so the striped wind fills the whole sky.",
     95.0, 8.0, -90.0, 36.0},
    {"aligned", "Nearly aligned rotator",
     "A pulsar whose magnetic axis is within 5 degrees of its spin axis, 11 turns per second, "
     "2e12 G. The beam circles the pole and never leaves an observer who sits near the axis: "
     "a wide, slowly modulated profile rather than a sharp pulse (the wide-profile pulsars "
     "such as PSR B0826-34). The current sheet is a thin, nearly flat equatorial disk.",
     95.0, 78.0, -90.0, 36.0},
}};

[[nodiscard]] NeutronStarSettings makeVariant(NeutronStarVariant variant) noexcept {
    NeutronStarSettings s;
    switch (variant) {
    case NeutronStarVariant::IsolatedCooling:
        s.spin_hz = 1.0f / 7.0f;
        s.magnetic_inclination_deg = 30.0f;
        s.log10_field_gauss = 13.0f;
        s.polar_temperature_k = 7.0e5f;
        s.conductivity_ratio = 0.05f;
        s.cap_temperature_k = 0.0f;
        s.outflow = 0.0f;
        s.wind_sheet = 0.0f;
        break;
    case NeutronStarVariant::YoungPulsar:
        break; // the defaults
    case NeutronStarVariant::MillisecondPulsar:
        s.spin_hz = 205.0f;
        s.magnetic_inclination_deg = 60.0f;
        s.log10_field_gauss = 8.3f;
        s.polar_temperature_k = 1.2e5f;
        s.conductivity_ratio = 0.8f;
        s.cap_temperature_k = 1.6e6f;
        break;
    case NeutronStarVariant::Magnetar:
        s.spin_hz = 1.0f / 6.0f;
        s.magnetic_inclination_deg = 20.0f;
        s.log10_field_gauss = 15.0f;
        s.polar_temperature_k = 4.0e6f;
        s.conductivity_ratio = 0.003f;
        s.cap_temperature_k = 9.0e6f;
        s.outflow = 0.0f;
        s.wind_sheet = 0.0f;
        s.twist_glow = 1.0f;
        s.twist_reach_radii = 4.0f;
        break;
    case NeutronStarVariant::OrthogonalPulsar:
        s.spin_hz = 20.0f;
        s.magnetic_inclination_deg = 88.0f;
        s.log10_field_gauss = 12.5f;
        break;
    case NeutronStarVariant::AlignedPulsar:
        s.spin_hz = 11.0f;
        s.magnetic_inclination_deg = 5.0f;
        s.log10_field_gauss = 12.3f;
        break;
    case NeutronStarVariant::AccretingPulsar:
        s.spin_hz = 401.0f;
        s.magnetic_inclination_deg = 12.0f;
        s.log10_field_gauss = 8.0f;
        s.polar_temperature_k = 1.5e5f;
        s.conductivity_ratio = 0.9f;
        s.cap_temperature_k = 0.0f;
        s.outflow = 0.0f;
        s.wind_sheet = 0.0f;
        s.accreting = true;
        // Inside corotation (2.6 stellar radii at 401 Hz), so matter reaches
        // the star rather than being flung off.
        s.log10_accretion_rate = -9.8f;
        break;
    }
    return s;
}

} // namespace

NeutronStarDerived deriveNeutronStar(NeutronStarSettings const& settings) noexcept {
    NeutronStarDerived d;
    double const radius = std::clamp(static_cast<double>(settings.radius_in_masses), 3.2, 12.0);
    double const radius_cm = radius * kMassCm;
    double const spin_hz = std::clamp(static_cast<double>(settings.spin_hz), 0.0, 1500.0);
    d.omega = 2.0 * std::numbers::pi * spin_hz * kMassSeconds;
    d.equatorial_speed = d.omega * radius;
    d.light_cylinder = d.omega > 1.0e-9 ? 1.0 / d.omega : 1.0e6;
    d.corotation_radius = d.omega > 1.0e-9 ? std::pow(d.omega, -2.0 / 3.0) : 1.0e6;
    d.surface_redshift = 1.0 / std::sqrt(1.0 - 2.0 / radius) - 1.0;
    d.cap_half_angle_deg =
        std::asin(std::sqrt(std::min(radius / d.light_cylinder, 1.0))) / kRadiansPerDegree;

    double const field = std::pow(10.0, std::clamp(static_cast<double>(settings.log10_field_gauss), 6.0, 16.0));
    double const moment = field * radius_cm * radius_cm * radius_cm; // G cm^3
    double const omega_cgs = 2.0 * std::numbers::pi * spin_hz;
    double const sine = std::sin(settings.magnetic_inclination_deg * kRadiansPerDegree);
    d.spin_down_erg_s = 2.0 / 3.0 * moment * moment * std::pow(omega_cgs, 4.0) * sine * sine /
                        (kLightSpeed * kLightSpeed * kLightSpeed);

    d.cap_reach = d.light_cylinder;
    if (settings.twist_glow > 0.0f) {
        d.cap_reach = std::min(d.cap_reach, static_cast<double>(settings.twist_reach_radii) * radius);
    }
    d.hottest_k = std::max(static_cast<double>(settings.polar_temperature_k),
                           d.cap_reach < 1.0e5 ? static_cast<double>(settings.cap_temperature_k) : 0.0);

    double emission_outer = radius;
    if (settings.outflow > 0.0f) {
        emission_outer = std::max(emission_outer, std::min(d.light_cylinder, 60.0 * radius));
    }
    if (settings.twist_glow > 0.0f) {
        emission_outer = std::max(emission_outer, 3.0 * settings.twist_reach_radii * radius);
    }
    if (settings.wind_sheet > 0.0f && d.omega > 1.0e-9) {
        d.sheet_outer = 6.0 * d.light_cylinder;
        emission_outer = std::max(emission_outer, d.sheet_outer);
    }

    if (settings.accreting) {
        double const rate = std::pow(10.0, std::clamp(static_cast<double>(settings.log10_accretion_rate), -14.0, -7.0)) *
                            kSolarMassPerYearGrams;
        double const alfven_cm = kAlfvenFactor *
                                 std::pow(std::pow(moment, 4.0) / (2.0 * kGravity * kMassGrams * rate * rate), 1.0 / 7.0);
        double magnetospheric = alfven_cm / kMassCm;
        // The disk cannot reach inside the star nor inside the last stable orbit.
        magnetospheric = std::max({magnetospheric, 1.02 * radius, 6.0});
        d.propeller = magnetospheric > d.corotation_radius;
        // The disk ends at the tidal truncation radius: Kepler's third law
        // for the binary, Eggleton's (1983) Roche lobe of the neutron star,
        // and truncation at 90 % of it (Whitehurst 1988).
        double const period_s =
            std::clamp(static_cast<double>(settings.orbital_period_h), 0.2, 500.0) * 3600.0;
        double const donor_g =
            std::clamp(static_cast<double>(settings.donor_mass_solar), 0.005, 3.0) * 1.989e33;
        double const separation_cm =
            std::cbrt(kGravity * (kMassGrams + donor_g) * period_s * period_s /
                      (4.0 * std::numbers::pi * std::numbers::pi));
        double const q = kMassGrams / donor_g;
        double const q23 = std::cbrt(q * q);
        double const roche_lobe = 0.49 * q23 / (0.6 * q23 + std::log(1.0 + std::cbrt(q)));
        d.binary_separation = separation_cm / kMassCm;
        double const outer = std::max(0.9 * roche_lobe * d.binary_separation, magnetospheric * 1.5);
        d.disk_outer_radius = outer;
        d.magnetospheric_radius = std::min(magnetospheric, outer / 1.5);
        // Shakura-Sunyaev: T^4 = 3 G M Mdot / (8 pi sigma r^3) (1 - sqrt(r_m / r)).
        d.disk_temperature_scale_k =
            std::pow(3.0 * kGravity * kMassGrams * rate /
                         (8.0 * std::numbers::pi * kStefanBoltzmann * kMassCm * kMassCm * kMassCm),
                     0.25);
        // The maximum of the profile lies at r = (49/36) r_m.
        double const peak_radius = 49.0 / 36.0 * d.magnetospheric_radius;
        d.inner_disk_temperature_k =
            d.disk_temperature_scale_k *
            std::pow((1.0 - std::sqrt(d.magnetospheric_radius / peak_radius)) /
                         (peak_radius * peak_radius * peak_radius),
                     0.25);
        if (!d.propeller) {
            d.accretion_luminosity_erg_s = kGravity * kMassGrams * rate / radius_cm;
            // The outer disk is heated by the X-rays of the inner parts and
            // of the star: it absorbs a fraction C of the accretion
            // luminosity per unit area at its distance,
            //     T_irr^4 = C L / (4 pi sigma r^2)
            // (de Jong, van Paradijs & Augusteijn 1996 measure C ~ 5e-3 in
            // low-mass X-ray binaries; Dubus et al. 1999).
            d.irradiation_k4_m2 =
                std::clamp(static_cast<double>(settings.irradiation_fraction), 0.0, 0.1) *
                d.accretion_luminosity_erg_s /
                (4.0 * std::numbers::pi * kStefanBoltzmann * kMassCm * kMassCm);
            // The streams land on the feet of the loaded field lines: a ring
            // (two half rings) between the reaches r_m and r_m (1 + width).
            double const width = std::clamp(static_cast<double>(settings.stream_width), 0.02, 1.0);
            double const cos_outer = std::sqrt(std::max(1.0 - radius / d.magnetospheric_radius, 0.0));
            double const cos_inner =
                std::sqrt(std::max(1.0 - radius / (d.magnetospheric_radius * (1.0 + width)), 0.0));
            double const area = 2.0 * std::numbers::pi * radius_cm * radius_cm *
                                std::max(cos_inner - cos_outer, 1.0e-3);
            d.spot_temperature_k = std::min(
                std::pow(d.accretion_luminosity_erg_s / (kStefanBoltzmann * area), 0.25), 5.0e7);
            d.hottest_k = std::max(d.hottest_k, d.spot_temperature_k);
            // Matter leaves the disk at a tenth of its orbital speed.
            d.stream_launch_speed = 0.1 / std::sqrt(d.magnetospheric_radius);
        }
        // Light paths are integrated only through the inner disk; beyond
        // this radius space is flat to a part in a thousand and the shader
        // crosses the disk's outer parts on a straight line.
        emission_outer = std::max(emission_outer,
                                  std::min(outer, std::max(600.0, 20.0 * d.magnetospheric_radius)));
    }
    d.emission_outer = emission_outer;
    return d;
}

NeutronStarVariantInfo const& neutronStarVariantInfo(NeutronStarVariant variant) noexcept {
    return kVariants[std::min(static_cast<std::size_t>(variant), kNeutronStarVariantCount - 1)];
}

NeutronStarSettings neutronStarVariant(NeutronStarVariant variant) noexcept {
    return makeVariant(variant);
}

} // namespace helios::examples

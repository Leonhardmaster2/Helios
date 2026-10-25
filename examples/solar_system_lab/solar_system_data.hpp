// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// J2000 mean orbital and physical elements of the Sun and the eight planets.
//
// Source: NASA NSSDCA planetary fact sheets, epoch J2000.0 (2000 January 1.5
// TT): the "Mean Orbital Elements (J2000)" table for the orbital fields and
// the sidereal orbit period, the bulk-parameters table for radius, mass,
// sidereal rotation and axial tilt, and the fact-sheet table for the mean
// temperature.  Saturn's and Neptune's fact sheets list semimajor axes in
// their orbital-parameters tables that disagree with their own mean-element
// tables (and with Kepler's third law for the stated periods); this table
// uses the mean-element values, 30.06896348 AU for Neptune and
// 9.53707032 AU for Saturn.
//
// These are mean two-body elements: good to arcminutes over decades and
// enough to place the planets in a visualization, not an ephemeris for
// navigation or occultation work.  Perturbations, the Moon, precession and
// relativity are not modelled.  The J2000 rows were checked against JPL
// Horizons DE441 while this table was written; the inner planets agree to
// 2-20 arcseconds at J2000.
#pragma once

#include "helios/math/vec.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace helios::examples {

inline constexpr double kAstronomicalUnitM = 1.495978707e11;
inline constexpr double kDaysPerJulianCentury = 36525.0;

struct SolarSystemBody {
    char const* name;
    double radius_m;
    double semi_major_axis_m;
    double eccentricity;
    double inclination_deg;
    double longitude_of_ascending_node_deg;
    double argument_of_periapsis_deg;
    double mean_anomaly_deg_at_j2000;
    double orbital_period_days;
    double rotation_period_hours; // sidereal; negative = retrograde
    double axial_tilt_deg;
    double mass_kg;
    double mean_temperature_k;
};

inline constexpr SolarSystemBody kSun{
    "Sun", 6.957e8, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 609.12, 7.25, 1.9885e30, 5772.0,
};

inline constexpr std::array<SolarSystemBody, 8> kSolarSystemPlanets{{
    // Mercury
    {"Mercury", 2.4397e6, 5.7909176e10, 0.205630, 7.00487, 48.33167, 29.12478, 174.79439,
     87.9691, 1407.6, 0.034, 3.3011e23, 440.0},
    // Venus
    {"Venus", 6.0518e6, 1.0820893e11, 0.00677323, 3.39471, 76.68069, 54.85229, 50.44675,
     224.701, -5832.5, 177.36, 4.8675e24, 737.0},
    // Earth
    {"Earth", 6.3710e6, 1.49597887e11, 0.01671022, 0.00005, -11.26064, 114.20783, 357.51716,
     365.256363, 23.9344696, 23.4392811, 5.97237e24, 288.0},
    // Mars
    {"Mars", 3.3895e6, 2.2793664e11, 0.09341233, 1.85061, 49.57854, 286.46230, 19.41248,
     686.980, 24.6229, 25.19, 6.4171e23, 210.0},
    // Jupiter
    {"Jupiter", 6.9911e7, 7.7841203e11, 0.04839266, 1.30530, 100.55615, 274.19770, 19.65053,
     4332.59, 9.925, 3.13, 1.8982e27, 165.0},
    // Saturn
    {"Saturn", 5.8232e7, 1.4267254e12, 0.05415060, 2.48446, 113.71504, 338.71690, 317.51238,
     10755.699, 10.656, 26.73, 5.6834e26, 134.0},
    // Uranus
    {"Uranus", 2.5362e7, 2.8709722e12, 0.04716771, 0.76986, 74.22988, 96.73436, 142.26794,
     30685.400, -17.24, 97.77, 8.6810e25, 76.0},
    // Neptune
    {"Neptune", 2.4622e7, 4.4982529e12, 0.00858587, 1.76917, 131.72169, 273.24966, 259.90868,
     60189.018, 16.11, 28.32, 1.02413e26, 72.0},
}};

// Heliocentric position in metres in the J2000 ecliptic frame (x toward the
// vernal equinox, z toward ecliptic north).  The two-body Kepler solution:
// M = M0 + n t, solve E - e sin E = M, then
//   r = a (1 - e cos E),
//   nu = atan2(sqrt(1 - e^2) sin E, cos E - e),
// and rotate by the argument of periapsis, the inclination and the ascending
// node.  `days_since_j2000` may be negative.
[[nodiscard]] inline math::Vec3d solarSystemPosition(SolarSystemBody const& body,
                                                     double days_since_j2000) noexcept {
    if (body.semi_major_axis_m <= 0.0 || body.orbital_period_days <= 0.0) {
        return {};
    }
    constexpr double kPi = std::numbers::pi;
    double const mean_motion = 2.0 * kPi / body.orbital_period_days;
    double mean_anomaly =
        std::fmod((body.mean_anomaly_deg_at_j2000 * kPi / 180.0) +
                      mean_motion * days_since_j2000,
                  2.0 * kPi);
    if (mean_anomaly > kPi) {
        mean_anomaly -= 2.0 * kPi;
    } else if (mean_anomaly < -kPi) {
        mean_anomaly += 2.0 * kPi;
    }

    double const e = std::clamp(body.eccentricity, 0.0, 0.99);
    double eccentric_anomaly = mean_anomaly;
    for (int iteration = 0; iteration < 16; ++iteration) {
        double const residual = eccentric_anomaly - e * std::sin(eccentric_anomaly) - mean_anomaly;
        double const derivative = 1.0 - e * std::cos(eccentric_anomaly);
        double const step = residual / derivative;
        eccentric_anomaly -= step;
        if (std::abs(step) < 1.0e-13) {
            break;
        }
    }

    double const sin_e = std::sin(eccentric_anomaly);
    double const cos_e = std::cos(eccentric_anomaly);
    double const true_anomaly = std::atan2(std::sqrt(1.0 - e * e) * sin_e, cos_e - e);
    double const radius = body.semi_major_axis_m * (1.0 - e * cos_e);

    double const inclination = body.inclination_deg * kPi / 180.0;
    double const node = body.longitude_of_ascending_node_deg * kPi / 180.0;
    double const argument = body.argument_of_periapsis_deg * kPi / 180.0;
    double const u = true_anomaly + argument;
    double const cos_u = std::cos(u);
    double const sin_u = std::sin(u);
    double const cos_i = std::cos(inclination);
    double const sin_i = std::sin(inclination);
    double const cos_node = std::cos(node);
    double const sin_node = std::sin(node);

    return {radius * (cos_node * cos_u - sin_node * sin_u * cos_i),
            radius * (sin_node * cos_u + cos_node * sin_u * cos_i),
            radius * (sin_u * sin_i)};
}

} // namespace helios::examples

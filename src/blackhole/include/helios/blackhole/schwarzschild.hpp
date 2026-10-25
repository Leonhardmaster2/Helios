// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Schwarzschild geometry helpers in geometric units (G = c = 1).
//
// `mass` is the gravitational length GM/c^2, so every radius returned by
// this file is expressed in the same length unit as `mass`.  The model is
// exact for the exterior vacuum of a spherical, non-rotating, uncharged
// black hole.  It intentionally does not model spin (Kerr), charge,
// dynamical space-time, accretion hydrodynamics, or radiative transfer.
#pragma once

#include <cmath>
#include <limits>

namespace helios::blackhole {

inline constexpr double kSqrt3 = 1.73205080756887729352744634150587236;

[[nodiscard]] constexpr double horizonRadius(double mass) noexcept {
    return 2.0 * mass;
}

[[nodiscard]] constexpr double photonSphereRadius(double mass) noexcept {
    return 3.0 * mass;
}

[[nodiscard]] constexpr double innermostStableCircularOrbitRadius(double mass) noexcept {
    return 6.0 * mass;
}

[[nodiscard]] constexpr double criticalImpactParameter(double mass) noexcept {
    return 3.0 * kSqrt3 * mass;
}

// Leading-order weak-field deflection.  This is an approximation, not the
// ray integrator used by the lab.  It is useful for tests and UI estimates
// only when |b| >> M.
[[nodiscard]] inline double weakDeflectionAngle(double mass, double impactParameter) noexcept {
    if (!std::isfinite(mass) || !std::isfinite(impactParameter) || mass <= 0.0 || impactParameter == 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return 4.0 * mass / std::abs(impactParameter);
}

// Schwarzschild lapse for a static observer.  For a photon emitted by that
// observer and received at rest at infinity, nu_infinity / nu_emitter is the
// returned value.  Static observers do not exist on or inside the horizon.
[[nodiscard]] inline double staticClockRateAtInfinity(double mass, double radius) noexcept {
    if (!std::isfinite(mass) || !std::isfinite(radius) || mass <= 0.0 || radius <= horizonRadius(mass)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return std::sqrt(1.0 - horizonRadius(mass) / radius);
}

// Conventional gravitational redshift z = nu_emitter / nu_infinity - 1 for
// a static source at `radius` and a static receiver at infinity.
[[nodiscard]] inline double staticRedshiftToInfinity(double mass, double radius) noexcept {
    const double clockRate = staticClockRateAtInfinity(mass, radius);
    if (!std::isfinite(clockRate) || clockRate <= 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return 1.0 / clockRate - 1.0;
}

struct SchwarzschildGeometry final {
    double mass = 1.0;

    [[nodiscard]] bool valid() const noexcept {
        return std::isfinite(mass) && mass > 0.0;
    }
    [[nodiscard]] constexpr double horizon() const noexcept {
        return horizonRadius(mass);
    }
    [[nodiscard]] constexpr double photonSphere() const noexcept {
        return photonSphereRadius(mass);
    }
    [[nodiscard]] constexpr double isco() const noexcept {
        return innermostStableCircularOrbitRadius(mass);
    }
    [[nodiscard]] constexpr double criticalImpact() const noexcept {
        return criticalImpactParameter(mass);
    }
};

} // namespace helios::blackhole

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors

#include "helios/blackhole/thin_disk.hpp"

#include "helios/blackhole/schwarzschild.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace helios::blackhole {
namespace {

constexpr double kPi = 3.14159265358979323846264338327950288;
constexpr double kSqrt6 = 2.44948974278317809819728407470589139;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Page & Thorne's bracket for a* = 0, written in x = sqrt(r/M):
//   B(x) = x - x0 + (sqrt(3)/2) ln[(x + sqrt3)(x0 - sqrt3) / ((x - sqrt3)(x0 + sqrt3))]
// with x0 = sqrt(6).  B and its first derivative vanish at the ISCO, so the
// closed form loses digits there; the Taylor series takes over close to x0.
[[nodiscard]] double pageThorneBracket(double radiusInMasses) noexcept {
    const double x = std::sqrt(radiusInMasses);
    const double delta = x - kSqrt6;
    if (delta < 1.0e-3) {
        return delta * delta * (kSqrt6 / 3.0 + delta * (-7.0 / 9.0 + delta * (kSqrt6 / 3.0)));
    }
    const double ratio = ((x + kSqrt3) * (kSqrt6 - kSqrt3)) / ((x - kSqrt3) * (kSqrt6 + kSqrt3));
    return delta + 0.5 * kSqrt3 * std::log(ratio);
}

[[nodiscard]] NovikovThornePeak findPeak() noexcept {
    // The flux is unimodal outside the ISCO; golden-section search on a
    // bracket that comfortably contains the maximum.
    constexpr double kInverseGolden = 0.61803398874989484820458683436563811;
    double low = 7.0;
    double high = 14.0;
    double first = high - kInverseGolden * (high - low);
    double second = low + kInverseGolden * (high - low);
    double firstFlux = novikovThorneFlux(first);
    double secondFlux = novikovThorneFlux(second);
    for (int iteration = 0; iteration < 96; ++iteration) {
        if (firstFlux < secondFlux) {
            low = first;
            first = second;
            firstFlux = secondFlux;
            second = low + kInverseGolden * (high - low);
            secondFlux = novikovThorneFlux(second);
        } else {
            high = second;
            second = first;
            secondFlux = firstFlux;
            first = high - kInverseGolden * (high - low);
            firstFlux = novikovThorneFlux(first);
        }
    }
    const double radius = 0.5 * (low + high);
    return NovikovThornePeak{radius, novikovThorneFlux(radius)};
}

} // namespace

double keplerianAngularVelocity(double radiusInMasses) noexcept {
    if (!std::isfinite(radiusInMasses) || radiusInMasses <= 0.0) {
        return kNaN;
    }
    return 1.0 / (radiusInMasses * std::sqrt(radiusInMasses));
}

double novikovThorneFlux(double radiusInMasses) noexcept {
    if (!std::isfinite(radiusInMasses) || radiusInMasses <= innermostStableCircularOrbitRadius(1.0)) {
        return 0.0;
    }
    const double r = radiusInMasses;
    return 3.0 / (8.0 * kPi) * pageThorneBracket(r) / ((r - 3.0) * r * r * std::sqrt(r));
}

NovikovThornePeak novikovThornePeak() noexcept {
    static const NovikovThornePeak peak = findPeak();
    return peak;
}

double diskTemperatureRatio(double radiusInMasses) noexcept {
    const double flux = novikovThorneFlux(radiusInMasses);
    if (flux <= 0.0) {
        return 0.0;
    }
    return std::sqrt(std::sqrt(flux / novikovThornePeak().flux));
}

DiskPhotonShift thinDiskPhotonShift(double observerRadiusInMasses, double emitterRadiusInMasses,
                                    double impactInMasses, double axialImpactInMasses) noexcept {
    const DiskPhotonShift invalid{kNaN, kNaN};
    if (!std::isfinite(emitterRadiusInMasses) || emitterRadiusInMasses <= photonSphereRadius(1.0) ||
        std::isnan(observerRadiusInMasses) || observerRadiusInMasses <= horizonRadius(1.0) ||
        !std::isfinite(impactInMasses) || !std::isfinite(axialImpactInMasses) ||
        std::abs(axialImpactInMasses) > std::abs(impactInMasses) * (1.0 + 1.0e-12)) {
        return invalid;
    }

    // Emitter four-velocity u = u^t (1, 0, 0, Omega) with u^t = 1/sqrt(1 - 3M/r);
    // photon p_t = -E, p_phi = L_z.  The static observer measures E / lapse.
    const double omega = keplerianAngularVelocity(emitterRadiusInMasses);
    const double observerLapse =
        std::isinf(observerRadiusInMasses) ? 1.0 : std::sqrt(1.0 - 2.0 / observerRadiusInMasses);
    const double denominator = 1.0 - omega * axialImpactInMasses;
    if (denominator <= 0.0) {
        return invalid;
    }
    const double frequencyRatio =
        std::sqrt(1.0 - 3.0 / emitterRadiusInMasses) / (observerLapse * denominator);

    // p_theta^2 = L^2 - L_z^2 on the equatorial plane and is unchanged by the
    // azimuthal boost into the gas frame, so cos = (|p_theta| / r) / E_emitter.
    const double polarMomentum =
        std::sqrt(std::max(impactInMasses * impactInMasses - axialImpactInMasses * axialImpactInMasses, 0.0));
    const double cosine = frequencyRatio * observerLapse * polarMomentum / emitterRadiusInMasses;
    return DiskPhotonShift{frequencyRatio, std::clamp(cosine, 0.0, 1.0)};
}

double limbDarkening(double cosEmissionAngle) noexcept {
    constexpr double kSlope = 2.06;
    const double mu = std::clamp(cosEmissionAngle, 0.0, 1.0);
    return (1.0 + kSlope * mu) / (1.0 + kSlope * (2.0 / 3.0));
}

} // namespace helios::blackhole

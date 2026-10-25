// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Radiometric helpers for a geometrically thin, optically thick accretion
// disk in the equatorial plane of a Schwarzschild black hole.
//
// Everything is expressed in geometric units with lengths in M = GM/c^2.
// The emitting gas follows prograde circular geodesics, which exist as stable
// orbits only at and outside the ISCO (r = 6M).  The radial energy release is
// the time-averaged zero-torque solution of Page & Thorne (1974), specialised
// to a* = 0.  The photon frequency shift uses the conserved photon energy and
// axial angular momentum, so it is exact for the stated emitter and observer
// rather than an approximation built from a flat-space line of sight.
//
// Scope limits: no spin, no disk thickness or self-irradiation, no returning
// radiation, no colour-correction (spectral hardening) factor, and no light
// travel-time delay between different parts of the image.
#pragma once

namespace helios::blackhole {

// Coordinate angular velocity dphi/dt of a circular equatorial geodesic,
// in units of 1/M: Omega = r^(-3/2).  Returns NaN for r <= 0.
[[nodiscard]] double keplerianAngularVelocity(double radiusInMasses) noexcept;

// Time-averaged energy flux emitted from one face of the disk, in units of
// Mdot / M^2 (Page & Thorne 1974, a* = 0).  Zero at and inside the ISCO, where
// the zero-torque boundary condition removes all dissipation.
[[nodiscard]] double novikovThorneFlux(double radiusInMasses) noexcept;

struct NovikovThornePeak final {
    double radiusInMasses = 0.0;
    double flux = 0.0;
};

// Location and value of the flux maximum (r is approximately 9.55 M).
[[nodiscard]] NovikovThornePeak novikovThornePeak() noexcept;

// Effective temperature relative to its maximum: (F(r) / F_peak)^(1/4).
[[nodiscard]] double diskTemperatureRatio(double radiusInMasses) noexcept;

struct DiskPhotonShift final {
    // g = nu_observer / nu_emitter.  Liouville invariance of I_nu / nu^3 makes
    // a black-body emitter of temperature T appear as a black body of g * T.
    double frequencyRatio = 0.0;

    // |cos| of the angle between the photon and the disk normal, measured in
    // the rest frame of the orbiting gas.  Used for limb darkening.
    double cosEmissionAngle = 0.0;
};

// Frequency shift and emission angle for a photon that leaves a prograde
// circular geodesic at `emitterRadiusInMasses` and is received by a static
// observer at `observerRadiusInMasses` (pass infinity for a distant observer).
//
// `impactInMasses` is b = L/E and `axialImpactInMasses` is lambda = L_z/E,
// where z is the disk's rotation axis and both are conserved along the ray.
// Returns NaN members when the emitter is not on a circular geodesic
// (r <= 3M), the observer is not outside the horizon, or |lambda| > |b|.
[[nodiscard]] DiskPhotonShift thinDiskPhotonShift(double observerRadiusInMasses, double emitterRadiusInMasses,
                                                  double impactInMasses, double axialImpactInMasses) noexcept;

// Angular distribution of the emergent intensity for an electron-scattering
// atmosphere, I(mu) proportional to 1 + 2.06 mu (Chandrasekhar), normalised so
// that the emergent flux equals that of an isotropic emitter.
[[nodiscard]] double limbDarkening(double cosEmissionAngle) noexcept;

} // namespace helios::blackhole

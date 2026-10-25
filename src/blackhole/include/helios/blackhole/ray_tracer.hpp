// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// CPU reference ray integration for a Schwarzschild black hole.
//
// The tracer integrates null geodesics in binary64 and is intended as the
// correctness foundation for BlackHoleLab, image regression tests, and a
// future GPU implementation.  Spherical symmetry reduces every 3D ray to
// its exact orbital plane; the solver integrates (r, dr/dlambda, psi) with
// adaptive Dormand-Prince RK5(4), then maps the orbit back to 3D.
//
// Scope limits: Schwarzschild exterior only.  The thin disk is an event
// surface, not a radiative-transfer or hydrodynamics model.  Coordinate time,
// frame dragging, polarization transport, absorption, emission, and spectral
// transfer are deliberately outside this v0.0.1 foundation.
#pragma once

#include "helios/blackhole/schwarzschild.hpp"
#include "helios/math/vec.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace helios::blackhole {

enum class RayTermination : std::uint8_t {
    Horizon,
    Escaped,
    AccretionDisk,
    MaxSteps,
    InvalidInput,
};

[[nodiscard]] std::string_view toString(RayTermination termination) noexcept;

// A camera sample expressed in the Schwarzschild-centered Cartesian frame.
// `direction` is a direction from the camera into the scene as measured by a
// local static observer.  It is normalized by the tracer.  Such an observer
// (and therefore this input model) is only defined outside r = 2M.
struct RayInitialCondition final {
    math::Vec3d position{0.0, 0.0, 10.0};
    math::Vec3d direction{0.0, 0.0, -1.0};
};

struct ThinDiskSettings final {
    bool enabled = false;

    // Unit is M (the geometry's gravitational length).  The default inner
    // edge is the Schwarzschild ISCO; this is a visualization convention,
    // not a claim that all physical disks truncate exactly there.
    double innerRadiusInMasses = 6.0;
    double outerRadiusInMasses = 30.0;

    // Disk plane in the same Cartesian frame as RayInitialCondition.
    // Normalization is performed during validation.
    math::Vec3d normal{0.0, 0.0, 1.0};

    // Suppresses a self-hit when the camera starts on the disk plane.
    double selfIntersectionEpsilonInMasses = 1.0e-7;
};

struct RayIntegrationSettings final {
    SchwarzschildGeometry geometry{};
    ThinDiskSettings disk{};

    // A ray is escaped only after it is outward-moving at this radius.
    double escapeRadiusInMasses = 100.0;

    // Stop just outside the coordinate horizon.  The integrated variables
    // remain regular there; the offset makes the terminal event unambiguous.
    double horizonOffsetFraction = 1.0e-7;

    // Adaptive affine-parameter step controls, all measured in M.
    double initialStepInMasses = 0.05;
    double minStepInMasses = 1.0e-7;
    double maxStepInMasses = 0.5;
    double absoluteTolerance = 1.0e-10;
    double relativeTolerance = 1.0e-9;

    // Additional geometric caps keep event detection reliable even if the
    // embedded RK error happens to be small.
    double maxRadialFractionPerStep = 0.025;
    double maxOrbitalAnglePerStep = 0.02;

    std::uint32_t maxSteps = 100000;
    double maxAffineLengthInMasses = 1000.0;

    bool recordTrajectory = false;
    std::uint32_t trajectoryStride = 1;
    std::size_t maxRecordedSamples = 8192;
};

struct RaySample final {
    double affineLength = 0.0;
    math::Vec3d position{};
    double radius = 0.0;
    double radialVelocity = 0.0;
    double orbitalAngle = 0.0;
    double absoluteNullResidual = 0.0;
};

struct DiskHit final {
    math::Vec3d position{};
    double radius = 0.0;
    double affineLength = 0.0;

    // True when the ray crosses from the side pointed to by the configured
    // disk normal to the opposite side.
    bool frontFace = false;
};

struct RayDiagnostics final {
    std::uint32_t acceptedSteps = 0;
    std::uint32_t rejectedSteps = 0;
    double affineLength = 0.0;
    double impactParameter = 0.0;

    // Energy at infinity for a photon assigned unit energy in the camera's
    // local static frame.  The equations normalize this conserved energy to
    // one; the value is retained for future radiometric calculations.
    double initialEnergyAtInfinity = 0.0;

    double closestRadius = 0.0;
    double totalOrbitalAngle = 0.0;
    double maxAbsoluteNullResidual = 0.0;
    double maxRelativeNullResidual = 0.0;
    double finalAbsoluteNullResidual = 0.0;
    double finalRelativeNullResidual = 0.0;
    double lastEmbeddedError = 0.0;
};

struct RayTraceResult final {
    RayTermination termination = RayTermination::InvalidInput;
    RayDiagnostics diagnostics{};
    RaySample terminalSample{};
    DiskHit diskHit{};
    bool hasDiskHit = false;
    std::vector<RaySample> trajectory{};
    std::string errorMessage{};
};

// Trace a backward camera ray until it reaches the horizon, escapes, hits the
// configured disk, or exhausts an integration limit.  Invalid inputs are
// reported in-band as InvalidInput with a diagnostic message; expected ray
// outcomes do not use exceptions.
[[nodiscard]] RayTraceResult traceSchwarzschildRay(RayInitialCondition const& initial,
                                                   RayIntegrationSettings const& settings = {});

} // namespace helios::blackhole

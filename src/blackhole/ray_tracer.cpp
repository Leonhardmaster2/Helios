// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors

#include "helios/blackhole/ray_tracer.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace helios::blackhole {
namespace {

struct OrbitBasis final {
    math::Vec3d radial{};
    math::Vec3d tangent{};
};

// All integration variables are dimensionless: x = r/M and s = lambda/M.
// This keeps one settings profile useful from stellar to supermassive holes.
struct State final {
    double radius = 0.0; // r / M
    double radialVelocity = 0.0;
    double angle = 0.0;
};

struct Derivative final {
    double radius = 0.0;
    double radialVelocity = 0.0;
    double angle = 0.0;
};

struct EmbeddedStep final {
    State fifthOrder{};
    State fourthOrder{};
    double normalizedError = 0.0;
};

[[nodiscard]] bool finite(double value) noexcept {
    return std::isfinite(value);
}

[[nodiscard]] bool finite(math::Vec3d const& value) noexcept {
    return finite(value.v[0]) && finite(value.v[1]) && finite(value.v[2]);
}

[[nodiscard]] State addScaled(State const& state, double step, Derivative const& derivative) noexcept {
    return State{state.radius + step * derivative.radius,
                 state.radialVelocity + step * derivative.radialVelocity,
                 state.angle + step * derivative.angle};
}

template <typename... Terms>
[[nodiscard]] State combine(State const& state, double step, Terms const&... terms) noexcept {
    Derivative sum{};
    auto accumulate = [&](double coefficient, Derivative const& derivative) {
        sum.radius += coefficient * derivative.radius;
        sum.radialVelocity += coefficient * derivative.radialVelocity;
        sum.angle += coefficient * derivative.angle;
    };
    (accumulate(terms.first, terms.second), ...);
    return addScaled(state, step, sum);
}

[[nodiscard]] Derivative evaluate(State const& state, double impactInMasses) noexcept {
    const double radius2 = state.radius * state.radius;
    const double impact2 = impactInMasses * impactInMasses;
    return Derivative{state.radialVelocity, impact2 * (state.radius - 3.0) / (radius2 * radius2),
                      impactInMasses / radius2};
}

[[nodiscard]] double componentError(double fourth, double fifth, double absoluteTolerance,
                                    double relativeTolerance) noexcept {
    const double scale = absoluteTolerance + relativeTolerance * std::max(std::abs(fourth), std::abs(fifth));
    return (fifth - fourth) / scale;
}

[[nodiscard]] EmbeddedStep dormandPrinceStep(State const& state, double step, double impactInMasses,
                                             double absoluteTolerance, double relativeTolerance) noexcept {
    using Term = std::pair<double, Derivative>;

    const Derivative k1 = evaluate(state, impactInMasses);
    const Derivative k2 = evaluate(combine(state, step, Term{1.0 / 5.0, k1}), impactInMasses);
    const Derivative k3 =
        evaluate(combine(state, step, Term{3.0 / 40.0, k1}, Term{9.0 / 40.0, k2}), impactInMasses);
    const Derivative k4 =
        evaluate(combine(state, step, Term{44.0 / 45.0, k1}, Term{-56.0 / 15.0, k2}, Term{32.0 / 9.0, k3}),
                 impactInMasses);
    const Derivative k5 =
        evaluate(combine(state, step, Term{19372.0 / 6561.0, k1}, Term{-25360.0 / 2187.0, k2},
                         Term{64448.0 / 6561.0, k3}, Term{-212.0 / 729.0, k4}),
                 impactInMasses);
    const Derivative k6 =
        evaluate(combine(state, step, Term{9017.0 / 3168.0, k1}, Term{-355.0 / 33.0, k2},
                         Term{46732.0 / 5247.0, k3}, Term{49.0 / 176.0, k4}, Term{-5103.0 / 18656.0, k5}),
                 impactInMasses);
    const Derivative k7 =
        evaluate(combine(state, step, Term{35.0 / 384.0, k1}, Term{500.0 / 1113.0, k3},
                         Term{125.0 / 192.0, k4}, Term{-2187.0 / 6784.0, k5}, Term{11.0 / 84.0, k6}),
                 impactInMasses);

    const State fifth = combine(state, step, Term{35.0 / 384.0, k1}, Term{500.0 / 1113.0, k3},
                                Term{125.0 / 192.0, k4}, Term{-2187.0 / 6784.0, k5}, Term{11.0 / 84.0, k6});
    const State fourth =
        combine(state, step, Term{5179.0 / 57600.0, k1}, Term{7571.0 / 16695.0, k3}, Term{393.0 / 640.0, k4},
                Term{-92097.0 / 339200.0, k5}, Term{187.0 / 2100.0, k6}, Term{1.0 / 40.0, k7});

    const double radiusError =
        componentError(fourth.radius, fifth.radius, absoluteTolerance, relativeTolerance);
    const double velocityError =
        componentError(fourth.radialVelocity, fifth.radialVelocity, absoluteTolerance, relativeTolerance);
    const double angleError = componentError(fourth.angle, fifth.angle, absoluteTolerance, relativeTolerance);
    const double normalizedError = std::sqrt(
        (radiusError * radiusError + velocityError * velocityError + angleError * angleError) / 3.0);
    return EmbeddedStep{fifth, fourth, normalizedError};
}

[[nodiscard]] bool finite(State const& state) noexcept {
    return finite(state.radius) && finite(state.radialVelocity) && finite(state.angle);
}

[[nodiscard]] math::Vec3d worldPosition(State const& state, OrbitBasis const& basis, double mass) noexcept {
    const double cosine = std::cos(state.angle);
    const double sine = std::sin(state.angle);
    return (basis.radial * cosine + basis.tangent * sine) * (state.radius * mass);
}

[[nodiscard]] double nullResidual(State const& state, double impactInMasses) noexcept {
    const double lapseSquared = 1.0 - 2.0 / state.radius;
    const double expectedRadialVelocitySquared =
        1.0 - lapseSquared * impactInMasses * impactInMasses / (state.radius * state.radius);
    return state.radialVelocity * state.radialVelocity - expectedRadialVelocitySquared;
}

[[nodiscard]] double relativeNullResidual(State const& state, double impactInMasses) noexcept {
    const double lapseSquared = 1.0 - 2.0 / state.radius;
    const double radialSquared = state.radialVelocity * state.radialVelocity;
    const double expected =
        1.0 - lapseSquared * impactInMasses * impactInMasses / (state.radius * state.radius);
    const double scale = std::max({1.0, std::abs(radialSquared), std::abs(expected)});
    return std::abs(radialSquared - expected) / scale;
}

[[nodiscard]] RaySample makeSample(State const& state, double affineInMasses, OrbitBasis const& basis,
                                   double mass, double impactInMasses) noexcept {
    return RaySample{affineInMasses * mass, worldPosition(state, basis, mass),
                     state.radius * mass,   state.radialVelocity,
                     state.angle,           std::abs(nullResidual(state, impactInMasses))};
}

void updateDiagnostics(RayTraceResult& result, State const& state, double affineInMasses,
                       double impactInMasses, double mass) noexcept {
    const double absoluteResidual = std::abs(nullResidual(state, impactInMasses));
    const double relativeResidual = relativeNullResidual(state, impactInMasses);
    result.diagnostics.affineLength = affineInMasses * mass;
    result.diagnostics.closestRadius = std::min(result.diagnostics.closestRadius, state.radius * mass);
    result.diagnostics.totalOrbitalAngle = std::abs(state.angle);
    result.diagnostics.maxAbsoluteNullResidual =
        std::max(result.diagnostics.maxAbsoluteNullResidual, absoluteResidual);
    result.diagnostics.maxRelativeNullResidual =
        std::max(result.diagnostics.maxRelativeNullResidual, relativeResidual);
    result.diagnostics.finalAbsoluteNullResidual = absoluteResidual;
    result.diagnostics.finalRelativeNullResidual = relativeResidual;
}

[[nodiscard]] math::Vec3d safePerpendicular(math::Vec3d const& radial) noexcept {
    const math::Vec3d axis =
        std::abs(radial.v[2]) < 0.9 ? math::Vec3d{0.0, 0.0, 1.0} : math::Vec3d{0.0, 1.0, 0.0};
    return math::normalized(math::cross(axis, radial));
}

[[nodiscard]] bool validateSettings(RayIntegrationSettings const& settings, std::string& error) {
    if (!settings.geometry.valid()) {
        error = "Schwarzschild mass must be finite and greater than zero";
        return false;
    }
    if (!finite(settings.escapeRadiusInMasses) || settings.escapeRadiusInMasses <= 2.0) {
        error = "escape radius must be finite and greater than 2M";
        return false;
    }
    if (!finite(settings.horizonOffsetFraction) || settings.horizonOffsetFraction < 0.0 ||
        settings.horizonOffsetFraction > 0.1) {
        error = "horizon offset fraction must be finite and in [0, 0.1]";
        return false;
    }
    if (!finite(settings.initialStepInMasses) || !finite(settings.minStepInMasses) ||
        !finite(settings.maxStepInMasses) || settings.initialStepInMasses <= 0.0 ||
        settings.minStepInMasses <= 0.0 || settings.maxStepInMasses < settings.minStepInMasses ||
        settings.initialStepInMasses < settings.minStepInMasses) {
        error = "adaptive steps must be finite, positive, and ordered min <= initial and min <= max";
        return false;
    }
    if (!finite(settings.absoluteTolerance) || !finite(settings.relativeTolerance) ||
        settings.absoluteTolerance <= 0.0 || settings.relativeTolerance <= 0.0) {
        error = "integration tolerances must be finite and greater than zero";
        return false;
    }
    if (!finite(settings.maxRadialFractionPerStep) || settings.maxRadialFractionPerStep <= 0.0 ||
        settings.maxRadialFractionPerStep > 0.5 || !finite(settings.maxOrbitalAnglePerStep) ||
        settings.maxOrbitalAnglePerStep <= 0.0 || settings.maxOrbitalAnglePerStep > 0.5) {
        error = "geometric step caps must be finite and in (0, 0.5]";
        return false;
    }
    if (settings.maxSteps == 0 || !finite(settings.maxAffineLengthInMasses) ||
        settings.maxAffineLengthInMasses <= 0.0) {
        error = "integration limits must be greater than zero";
        return false;
    }
    if (settings.recordTrajectory && (settings.trajectoryStride == 0 || settings.maxRecordedSamples == 0)) {
        error = "trajectory recording requires a non-zero stride and capacity";
        return false;
    }
    if (settings.disk.enabled) {
        if (!finite(settings.disk.innerRadiusInMasses) || !finite(settings.disk.outerRadiusInMasses) ||
            settings.disk.innerRadiusInMasses <= 2.0 ||
            settings.disk.outerRadiusInMasses <= settings.disk.innerRadiusInMasses) {
            error = "disk radii must be finite and satisfy 2M < inner < outer";
            return false;
        }
        if (!finite(settings.disk.normal) || math::lengthSq(settings.disk.normal) <= 0.0) {
            error = "disk normal must be finite and non-zero";
            return false;
        }
        if (!finite(settings.disk.selfIntersectionEpsilonInMasses) ||
            settings.disk.selfIntersectionEpsilonInMasses < 0.0) {
            error = "disk self-intersection epsilon must be finite and non-negative";
            return false;
        }
        if (settings.escapeRadiusInMasses <= settings.disk.outerRadiusInMasses) {
            error = "escape radius must lie outside the enabled disk";
            return false;
        }
    }
    return true;
}

template <typename ValueFunction>
[[nodiscard]] std::pair<double, State> refineEvent(State const& start, double step, double impactInMasses,
                                                   double lowValue, double highValue,
                                                   ValueFunction&& valueFunction) {
    double low = 0.0;
    double high = 1.0;
    State highState = dormandPrinceStep(start, step, impactInMasses, 1.0e-13, 1.0e-13).fifthOrder;

    // Event refinement is independent of the caller's visual tolerance.  48
    // bisections make interpolation error negligible relative to RK error.
    for (int iteration = 0; iteration < 48; ++iteration) {
        const double middle = 0.5 * (low + high);
        const State middleState =
            dormandPrinceStep(start, step * middle, impactInMasses, 1.0e-13, 1.0e-13).fifthOrder;
        const double middleValue = valueFunction(middleState);
        const bool middleOnLowSide =
            (lowValue <= 0.0 && middleValue <= 0.0) || (lowValue >= 0.0 && middleValue >= 0.0);
        if (middleOnLowSide) {
            low = middle;
            lowValue = middleValue;
        } else {
            high = middle;
            highValue = middleValue;
            highState = middleState;
        }
    }
    (void)highValue;
    return {high, highState};
}

struct DiskEvent final {
    bool occurred = false;
    double fraction = 1.0;
    State state{};
    bool frontFace = false;
};

[[nodiscard]] DiskEvent findDiskEvent(State const& start, State const& end, double step,
                                      double affineInMasses, double impactInMasses, OrbitBasis const& basis,
                                      math::Vec3d const& diskNormal, ThinDiskSettings const& disk,
                                      double mass) {
    const math::Vec3d startPosition = worldPosition(start, basis, mass);
    const math::Vec3d endPosition = worldPosition(end, basis, mass);
    const double startHeight = math::dot(startPosition, diskNormal);
    const double endHeight = math::dot(endPosition, diskNormal);
    const double planeTolerance =
        std::max(1.0e-12 * mass, disk.selfIntersectionEpsilonInMasses * mass * 1.0e-3);
    const double selfEpsilon = disk.selfIntersectionEpsilonInMasses;

    auto radiusInDisk = [&](double radius) {
        return radius >= disk.innerRadiusInMasses && radius <= disk.outerRadiusInMasses;
    };

    if (std::abs(startHeight) <= planeTolerance && affineInMasses >= selfEpsilon &&
        radiusInDisk(start.radius)) {
        return DiskEvent{true, 0.0, start, false};
    }

    if (std::abs(startHeight) <= planeTolerance && std::abs(endHeight) <= planeTolerance) {
        // An exactly coplanar photon inhabits the idealized disk surface.
        // Select the earliest non-self point in the radial annulus.
        if (step > 0.0 && affineInMasses < selfEpsilon && affineInMasses + step >= selfEpsilon) {
            const double fraction = (selfEpsilon - affineInMasses) / step;
            const State candidate =
                dormandPrinceStep(start, step * fraction, impactInMasses, 1.0e-13, 1.0e-13).fifthOrder;
            if (radiusInDisk(candidate.radius)) {
                return DiskEvent{true, fraction, candidate, false};
            }
        }

        double boundary = 0.0;
        bool crossesBoundary = false;
        if (start.radius > disk.outerRadiusInMasses && end.radius <= disk.outerRadiusInMasses) {
            boundary = disk.outerRadiusInMasses;
            crossesBoundary = true;
        } else if (start.radius < disk.innerRadiusInMasses && end.radius >= disk.innerRadiusInMasses) {
            boundary = disk.innerRadiusInMasses;
            crossesBoundary = true;
        }

        if (crossesBoundary) {
            auto radialBoundary = [boundary](State const& candidate) { return candidate.radius - boundary; };
            const auto [fraction, candidate] = refineEvent(
                start, step, impactInMasses, start.radius - boundary, end.radius - boundary, radialBoundary);
            if (affineInMasses + fraction * step + 1.0e-15 >= selfEpsilon) {
                return DiskEvent{true, fraction, candidate, false};
            }
        }
        return {};
    }

    if ((startHeight > 0.0 && endHeight > 0.0) || (startHeight < 0.0 && endHeight < 0.0) ||
        startHeight == endHeight) {
        return {};
    }

    const double linearFraction = std::clamp(startHeight / (startHeight - endHeight), 0.0, 1.0);
    if (affineInMasses + linearFraction * step + 1.0e-15 < selfEpsilon) {
        return {};
    }

    auto signedHeight = [&](State const& state) {
        return math::dot(worldPosition(state, basis, mass), diskNormal);
    };
    const auto [fraction, state] =
        refineEvent(start, step, impactInMasses, startHeight, endHeight, signedHeight);
    if (!radiusInDisk(state.radius)) {
        return {};
    }
    return DiskEvent{true, fraction, state, startHeight > endHeight};
}

void recordSampleIfRequested(RayTraceResult& result, RayIntegrationSettings const& settings,
                             RaySample const& sample, bool force) {
    if (!settings.recordTrajectory || result.trajectory.size() >= settings.maxRecordedSamples) {
        return;
    }
    if (force || result.diagnostics.acceptedSteps % settings.trajectoryStride == 0) {
        result.trajectory.push_back(sample);
    }
}

} // namespace

std::string_view toString(RayTermination termination) noexcept {
    switch (termination) {
    case RayTermination::Horizon:
        return "horizon";
    case RayTermination::Escaped:
        return "escaped";
    case RayTermination::AccretionDisk:
        return "accretion_disk";
    case RayTermination::MaxSteps:
        return "max_steps";
    case RayTermination::InvalidInput:
        return "invalid_input";
    }
    return "unknown";
}

RayTraceResult traceSchwarzschildRay(RayInitialCondition const& initial,
                                     RayIntegrationSettings const& settings) {
    RayTraceResult result{};
    if (!validateSettings(settings, result.errorMessage)) {
        return result;
    }
    if (!finite(initial.position) || !finite(initial.direction)) {
        result.errorMessage = "ray position and direction must be finite";
        return result;
    }

    const double mass = settings.geometry.mass;
    const double initialRadius = math::length(initial.position);
    const double directionLength = math::length(initial.direction);
    const double horizonStop = 2.0 * (1.0 + settings.horizonOffsetFraction);
    if (!finite(initialRadius) || initialRadius / mass <= horizonStop) {
        result.errorMessage = "a static-observer ray must start outside the horizon stop surface";
        return result;
    }
    if (!finite(directionLength) || directionLength <= 0.0) {
        result.errorMessage = "ray direction must be non-zero";
        return result;
    }

    const math::Vec3d radial = initial.position / initialRadius;
    const math::Vec3d direction = initial.direction / directionLength;
    const double radialDirection = std::clamp(math::dot(direction, radial), -1.0, 1.0);
    const math::Vec3d tangentialVector = direction - radial * radialDirection;
    const double tangentialDirection = math::length(tangentialVector);
    const math::Vec3d tangent =
        tangentialDirection > 1.0e-14 ? tangentialVector / tangentialDirection : safePerpendicular(radial);
    const OrbitBasis basis{radial, tangent};

    const double initialRadiusInMasses = initialRadius / mass;
    const double lapse = std::sqrt(1.0 - 2.0 / initialRadiusInMasses);
    const double impactInMasses = initialRadiusInMasses * tangentialDirection / lapse;
    State state{initialRadiusInMasses, radialDirection, 0.0};
    double affineInMasses = 0.0;
    double step =
        std::clamp(settings.initialStepInMasses, settings.minStepInMasses, settings.maxStepInMasses);
    const math::Vec3d diskNormal =
        settings.disk.enabled ? math::normalized(settings.disk.normal) : settings.disk.normal;

    result.diagnostics.impactParameter = impactInMasses * mass;
    result.diagnostics.initialEnergyAtInfinity = lapse;
    result.diagnostics.closestRadius = initialRadius;
    updateDiagnostics(result, state, affineInMasses, impactInMasses, mass);
    result.terminalSample = makeSample(state, affineInMasses, basis, mass, impactInMasses);
    recordSampleIfRequested(result, settings, result.terminalSample, true);

    if (state.radius >= settings.escapeRadiusInMasses && state.radialVelocity > 0.0) {
        result.termination = RayTermination::Escaped;
        return result;
    }

    for (std::uint32_t attempt = 0; attempt < settings.maxSteps; ++attempt) {
        const double remaining = settings.maxAffineLengthInMasses - affineInMasses;
        if (remaining <= 0.0) {
            result.termination = RayTermination::MaxSteps;
            result.errorMessage = "maximum affine length reached";
            return result;
        }

        double boundedStep = std::min({step, settings.maxStepInMasses, remaining});
        if (std::abs(state.radialVelocity) > 1.0e-15) {
            boundedStep = std::min(boundedStep, settings.maxRadialFractionPerStep * state.radius /
                                                    std::abs(state.radialVelocity));
        }
        const double angularRate = std::abs(impactInMasses) / (state.radius * state.radius);
        if (angularRate > 0.0) {
            boundedStep = std::min(boundedStep, settings.maxOrbitalAnglePerStep / angularRate);
        }
        boundedStep = std::max(std::min(boundedStep, remaining), settings.minStepInMasses);
        if (boundedStep > remaining) {
            boundedStep = remaining;
        }

        const EmbeddedStep trial = dormandPrinceStep(state, boundedStep, impactInMasses,
                                                     settings.absoluteTolerance, settings.relativeTolerance);
        result.diagnostics.lastEmbeddedError = trial.normalizedError;
        if (!finite(trial.fifthOrder) || !finite(trial.normalizedError)) {
            result.termination = RayTermination::MaxSteps;
            result.errorMessage = "non-finite state produced by the integrator";
            return result;
        }

        if (trial.normalizedError > 1.0) {
            ++result.diagnostics.rejectedSteps;
            if (boundedStep <= settings.minStepInMasses * (1.0 + 1.0e-12)) {
                result.termination = RayTermination::MaxSteps;
                result.errorMessage = "adaptive integration reached its minimum step";
                return result;
            }
            const double shrink = std::clamp(0.9 * std::pow(trial.normalizedError, -0.25), 0.1, 0.5);
            step = std::max(settings.minStepInMasses, boundedStep * shrink);
            continue;
        }

        ++result.diagnostics.acceptedSteps;
        const State previous = state;
        const double previousAffine = affineInMasses;
        const State accepted = trial.fifthOrder;

        enum class EventKind : std::uint8_t { None, Horizon, Escape, Disk };
        EventKind event = EventKind::None;
        double eventFraction = 2.0;
        State eventState = accepted;
        DiskEvent diskEvent{};

        if (previous.radius > horizonStop && accepted.radius <= horizonStop) {
            auto value = [horizonStop](State const& candidate) { return candidate.radius - horizonStop; };
            const auto refined =
                refineEvent(previous, boundedStep, impactInMasses, previous.radius - horizonStop,
                            accepted.radius - horizonStop, value);
            event = EventKind::Horizon;
            eventFraction = refined.first;
            eventState = refined.second;
        }

        if (previous.radius < settings.escapeRadiusInMasses &&
            accepted.radius >= settings.escapeRadiusInMasses && accepted.radialVelocity > 0.0) {
            auto value = [&](State const& candidate) {
                return candidate.radius - settings.escapeRadiusInMasses;
            };
            const auto refined = refineEvent(previous, boundedStep, impactInMasses,
                                             previous.radius - settings.escapeRadiusInMasses,
                                             accepted.radius - settings.escapeRadiusInMasses, value);
            if (refined.first < eventFraction) {
                event = EventKind::Escape;
                eventFraction = refined.first;
                eventState = refined.second;
            }
        }

        // A caller may deliberately place the camera beyond the configured
        // escape sphere and launch a ray inward.  If that ray turns around
        // without ever entering the sphere, its outward turning point is the
        // earliest unambiguous escape event.
        if (previous.radius >= settings.escapeRadiusInMasses &&
            accepted.radius >= settings.escapeRadiusInMasses && previous.radialVelocity <= 0.0 &&
            accepted.radialVelocity > 0.0) {
            auto value = [](State const& candidate) { return candidate.radialVelocity; };
            const auto refined = refineEvent(previous, boundedStep, impactInMasses, previous.radialVelocity,
                                             accepted.radialVelocity, value);
            if (refined.first < eventFraction) {
                event = EventKind::Escape;
                eventFraction = refined.first;
                eventState = refined.second;
            }
        }

        if (settings.disk.enabled) {
            diskEvent = findDiskEvent(previous, accepted, boundedStep, previousAffine, impactInMasses, basis,
                                      diskNormal, settings.disk, mass);
            if (diskEvent.occurred && diskEvent.fraction < eventFraction) {
                event = EventKind::Disk;
                eventFraction = diskEvent.fraction;
                eventState = diskEvent.state;
            }
        }

        if (event != EventKind::None) {
            state = eventState;
            affineInMasses = previousAffine + eventFraction * boundedStep;
            updateDiagnostics(result, state, affineInMasses, impactInMasses, mass);
            result.terminalSample = makeSample(state, affineInMasses, basis, mass, impactInMasses);
            recordSampleIfRequested(result, settings, result.terminalSample, true);
            if (event == EventKind::Horizon) {
                result.termination = RayTermination::Horizon;
            } else if (event == EventKind::Escape) {
                result.termination = RayTermination::Escaped;
            } else {
                result.termination = RayTermination::AccretionDisk;
                result.hasDiskHit = true;
                result.diskHit = DiskHit{result.terminalSample.position, result.terminalSample.radius,
                                         result.terminalSample.affineLength, diskEvent.frontFace};
            }
            return result;
        }

        state = accepted;
        affineInMasses += boundedStep;
        updateDiagnostics(result, state, affineInMasses, impactInMasses, mass);
        result.terminalSample = makeSample(state, affineInMasses, basis, mass, impactInMasses);
        recordSampleIfRequested(result, settings, result.terminalSample, false);

        if (affineInMasses >= settings.maxAffineLengthInMasses) {
            result.termination = RayTermination::MaxSteps;
            result.errorMessage = "maximum affine length reached";
            return result;
        }

        const double growth = trial.normalizedError > 0.0
                                  ? std::clamp(0.9 * std::pow(trial.normalizedError, -0.2), 0.2, 5.0)
                                  : 5.0;
        step = std::clamp(boundedStep * growth, settings.minStepInMasses, settings.maxStepInMasses);
    }

    result.termination = RayTermination::MaxSteps;
    result.errorMessage = "maximum integration step count reached";
    return result;
}

} // namespace helios::blackhole

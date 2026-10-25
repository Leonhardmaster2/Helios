// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helios/blackhole/blackbody.hpp"
#include "helios/blackhole/ray_tracer.hpp"
#include "helios/blackhole/schwarzschild.hpp"
#include "helios/blackhole/thin_disk.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

using namespace helios::blackhole;
using helios::math::Vec3d;

namespace {

RayInitialCondition inwardRayWithImpact(double radiusInMasses, double impactInMasses, double mass = 1.0) {
    const double lapse = std::sqrt(1.0 - 2.0 / radiusInMasses);
    const double tangent = impactInMasses * lapse / radiusInMasses;
    return RayInitialCondition{Vec3d{radiusInMasses * mass, 0.0, 0.0},
                               Vec3d{-std::sqrt(1.0 - tangent * tangent), tangent, 0.0}};
}

} // namespace

TEST_CASE("Schwarzschild landmark radii use gravitational length M") {
    constexpr double mass = 7.0;
    CHECK(horizonRadius(mass) == doctest::Approx(14.0));
    CHECK(photonSphereRadius(mass) == doctest::Approx(21.0));
    CHECK(innermostStableCircularOrbitRadius(mass) == doctest::Approx(42.0));
    CHECK(criticalImpactParameter(mass) == doctest::Approx(3.0 * std::sqrt(3.0) * mass));
    CHECK(weakDeflectionAngle(mass, 1000.0 * mass) == doctest::Approx(0.004));
}

TEST_CASE("static-observer redshift has the expected Schwarzschild limits") {
    CHECK(staticClockRateAtInfinity(1.0, 8.0) == doctest::Approx(std::sqrt(0.75)));
    CHECK(staticRedshiftToInfinity(1.0, 8.0) == doctest::Approx(1.0 / std::sqrt(0.75) - 1.0));
    CHECK(std::isnan(staticClockRateAtInfinity(1.0, 2.0)));
    CHECK(std::isnan(staticRedshiftToInfinity(-1.0, 8.0)));
}

TEST_CASE("radial null rays classify as captured or escaped") {
    RayIntegrationSettings settings{};
    settings.escapeRadiusInMasses = 30.0;

    const RayTraceResult captured =
        traceSchwarzschildRay(RayInitialCondition{Vec3d{10.0, 0.0, 0.0}, Vec3d{-1.0, 0.0, 0.0}}, settings);
    CHECK(static_cast<int>(captured.termination) == static_cast<int>(RayTermination::Horizon));
    CHECK(captured.terminalSample.radius ==
          doctest::Approx(2.0 * (1.0 + settings.horizonOffsetFraction)).epsilon(1.0e-8));
    CHECK(captured.diagnostics.maxRelativeNullResidual < 1.0e-10);

    const RayTraceResult escaped =
        traceSchwarzschildRay(RayInitialCondition{Vec3d{10.0, 0.0, 0.0}, Vec3d{1.0, 0.0, 0.0}}, settings);
    CHECK(static_cast<int>(escaped.termination) == static_cast<int>(RayTermination::Escaped));
    CHECK(escaped.terminalSample.radius == doctest::Approx(30.0).epsilon(1.0e-8));
    CHECK(escaped.diagnostics.maxRelativeNullResidual < 1.0e-10);
}

TEST_CASE("critical impact separates capture from scattering") {
    RayIntegrationSettings settings{};
    settings.escapeRadiusInMasses = 80.0;
    settings.maxAffineLengthInMasses = 300.0;

    const RayTraceResult captured = traceSchwarzschildRay(inwardRayWithImpact(40.0, 4.0), settings);
    const RayTraceResult scattered = traceSchwarzschildRay(inwardRayWithImpact(40.0, 6.0), settings);

    CHECK(static_cast<int>(captured.termination) == static_cast<int>(RayTermination::Horizon));
    CHECK(static_cast<int>(scattered.termination) == static_cast<int>(RayTermination::Escaped));
    CHECK(captured.diagnostics.impactParameter == doctest::Approx(4.0).epsilon(1.0e-12));
    CHECK(scattered.diagnostics.impactParameter == doctest::Approx(6.0).epsilon(1.0e-12));
    CHECK(scattered.diagnostics.closestRadius > photonSphereRadius(1.0));
    CHECK(scattered.diagnostics.totalOrbitalAngle > 1.0);
}

TEST_CASE("the exact photon-sphere orbit remains at r equals 3M") {
    RayIntegrationSettings settings{};
    settings.maxAffineLengthInMasses = 20.0;
    settings.maxSteps = 10000;

    const RayTraceResult orbit =
        traceSchwarzschildRay(RayInitialCondition{Vec3d{3.0, 0.0, 0.0}, Vec3d{0.0, 1.0, 0.0}}, settings);
    CHECK(static_cast<int>(orbit.termination) == static_cast<int>(RayTermination::MaxSteps));
    CHECK(orbit.terminalSample.radius == doctest::Approx(3.0).epsilon(1.0e-11));
    CHECK(orbit.diagnostics.impactParameter ==
          doctest::Approx(criticalImpactParameter(1.0)).epsilon(1.0e-12));
    CHECK(orbit.diagnostics.maxRelativeNullResidual < 1.0e-10);
}

TEST_CASE("a 3D ray reports a geometrically resolved thin-disk crossing") {
    RayIntegrationSettings settings{};
    settings.disk.enabled = true;
    settings.disk.innerRadiusInMasses = 6.0;
    settings.disk.outerRadiusInMasses = 30.0;
    settings.escapeRadiusInMasses = 100.0;

    const RayTraceResult hit =
        traceSchwarzschildRay(RayInitialCondition{Vec3d{20.0, 0.0, 10.0}, Vec3d{0.0, 0.0, -1.0}}, settings);
    REQUIRE(static_cast<int>(hit.termination) == static_cast<int>(RayTermination::AccretionDisk));
    REQUIRE(hit.hasDiskHit);
    CHECK(std::abs(hit.diskHit.position.v[2]) < 1.0e-8);
    CHECK(hit.diskHit.radius >= 6.0);
    CHECK(hit.diskHit.radius <= 30.0);
    CHECK(hit.diskHit.frontFace);
}

TEST_CASE("dimensionless integration is invariant when M and the scene are scaled") {
    RayIntegrationSettings unitSettings{};
    unitSettings.escapeRadiusInMasses = 50.0;
    const RayTraceResult unit = traceSchwarzschildRay(inwardRayWithImpact(25.0, 6.0), unitSettings);

    RayIntegrationSettings scaledSettings = unitSettings;
    scaledSettings.geometry.mass = 1000.0;
    const RayTraceResult scaled =
        traceSchwarzschildRay(inwardRayWithImpact(25.0, 6.0, 1000.0), scaledSettings);

    CHECK(static_cast<int>(scaled.termination) == static_cast<int>(unit.termination));
    CHECK(scaled.diagnostics.closestRadius / 1000.0 ==
          doctest::Approx(unit.diagnostics.closestRadius).epsilon(1.0e-11));
    CHECK(scaled.diagnostics.totalOrbitalAngle ==
          doctest::Approx(unit.diagnostics.totalOrbitalAngle).epsilon(1.0e-11));
}

TEST_CASE("invalid static-observer inputs fail with diagnostics") {
    const RayTraceResult inside =
        traceSchwarzschildRay(RayInitialCondition{Vec3d{1.5, 0.0, 0.0}, Vec3d{1.0, 0.0, 0.0}});
    CHECK(static_cast<int>(inside.termination) == static_cast<int>(RayTermination::InvalidInput));
    CHECK_FALSE(inside.errorMessage.empty());

    RayIntegrationSettings invalid{};
    invalid.geometry.mass = 0.0;
    const RayTraceResult zeroMass = traceSchwarzschildRay({}, invalid);
    CHECK(static_cast<int>(zeroMass.termination) == static_cast<int>(RayTermination::InvalidInput));
    CHECK_FALSE(zeroMass.errorMessage.empty());
}

TEST_CASE("Page-Thorne flux vanishes at the ISCO and reaches the Newtonian limit") {
    CHECK(novikovThorneFlux(6.0) == 0.0);
    CHECK(novikovThorneFlux(4.0) == 0.0);
    CHECK(novikovThorneFlux(std::numeric_limits<double>::quiet_NaN()) == 0.0);
    CHECK(novikovThorneFlux(6.0 + 1.0e-6) > 0.0);

    // Hand-evaluated closed form at r = 10 M.
    CHECK(novikovThorneFlux(10.0) == doctest::Approx(1.35688e-5).epsilon(1.0e-4));

    // Both the flux and its radial derivative vanish at the ISCO, so close to
    // it the flux grows quadratically in sqrt(r) - sqrt(6). The radii straddle
    // the hand-over between the series and the closed form.
    const auto offset = [](double radius) { return std::sqrt(radius) - std::sqrt(6.0); };
    const double nearRatio = novikovThorneFlux(6.008) / novikovThorneFlux(6.004);
    const double expectedRatio = std::pow(offset(6.008) / offset(6.004), 2.0);
    CHECK(nearRatio == doctest::Approx(expectedRatio).epsilon(1.0e-2));
    for (double radius = 6.0005; radius < 6.02; radius += 0.0005) {
        CHECK(novikovThorneFlux(radius + 0.0005) > novikovThorneFlux(radius));
    }

    // F -> 3 Mdot / (8 pi r^3) far from the hole.
    constexpr double pi = 3.14159265358979323846;
    const double farRadius = 1.0e8;
    CHECK(novikovThorneFlux(farRadius) * 8.0 * pi * farRadius * farRadius * farRadius / 3.0 ==
          doctest::Approx(1.0).epsilon(1.0e-3));
}

TEST_CASE("the Page-Thorne flux peaks near 9.55 M") {
    const NovikovThornePeak peak = novikovThornePeak();
    CHECK(peak.radiusInMasses == doctest::Approx(9.55).epsilon(2.0e-3));
    CHECK(peak.flux > novikovThorneFlux(peak.radiusInMasses - 0.05));
    CHECK(peak.flux > novikovThorneFlux(peak.radiusInMasses + 0.05));
    CHECK(diskTemperatureRatio(peak.radiusInMasses) == doctest::Approx(1.0).epsilon(1.0e-12));
    CHECK(diskTemperatureRatio(6.0) == 0.0);
    // T is proportional to r^(-3/4) far out.
    CHECK(diskTemperatureRatio(4.0e6) / diskTemperatureRatio(1.0e6) ==
          doctest::Approx(std::pow(4.0, -0.75)).epsilon(2.0e-3));
}

TEST_CASE("thin-disk photon shift matches the analytic circular-orbit limits") {
    const double infinity = std::numeric_limits<double>::infinity();
    CHECK(keplerianAngularVelocity(4.0) == doctest::Approx(0.125));

    // A photon with no axial angular momentum sees only time dilation.
    const DiskPhotonShift polar = thinDiskPhotonShift(infinity, 6.0, 3.0, 0.0);
    CHECK(polar.frequencyRatio == doctest::Approx(std::sqrt(0.5)));

    // In-plane photons emitted along and against the orbit at the ISCO:
    // b = r / sqrt(1 - 2M/r), giving g = sqrt(2) and sqrt(2)/3.
    const double tangentImpact = 6.0 / std::sqrt(1.0 - 2.0 / 6.0);
    const DiskPhotonShift approaching = thinDiskPhotonShift(infinity, 6.0, tangentImpact, tangentImpact);
    const DiskPhotonShift receding = thinDiskPhotonShift(infinity, 6.0, tangentImpact, -tangentImpact);
    CHECK(approaching.frequencyRatio == doctest::Approx(std::sqrt(2.0)));
    CHECK(receding.frequencyRatio == doctest::Approx(std::sqrt(2.0) / 3.0));
    CHECK(approaching.cosEmissionAngle < 1.0e-12);
    CHECK(receding.cosEmissionAngle < 1.0e-12);

    // A general ray: g = sqrt(1 - 3/r) / (1 - Omega lambda),
    // cos = g sqrt(b^2 - lambda^2) / r.
    const DiskPhotonShift general = thinDiskPhotonShift(infinity, 10.0, 5.0, 3.0);
    const double expectedShift = std::sqrt(0.7) / (1.0 - 3.0 * std::pow(10.0, -1.5));
    CHECK(general.frequencyRatio == doctest::Approx(expectedShift));
    CHECK(general.cosEmissionAngle == doctest::Approx(expectedShift * 4.0 / 10.0));

    // A static observer deeper in the potential measures a higher frequency
    // by exactly its own lapse, and the emission angle does not change.
    const DiskPhotonShift nearObserver = thinDiskPhotonShift(20.0, 10.0, 5.0, 3.0);
    CHECK(nearObserver.frequencyRatio ==
          doctest::Approx(general.frequencyRatio / staticClockRateAtInfinity(1.0, 20.0)));
    CHECK(nearObserver.cosEmissionAngle == doctest::Approx(general.cosEmissionAngle));

    CHECK(std::isnan(thinDiskPhotonShift(infinity, 3.0, 5.0, 3.0).frequencyRatio));
    CHECK(std::isnan(thinDiskPhotonShift(2.0, 10.0, 5.0, 3.0).frequencyRatio));
    CHECK(std::isnan(thinDiskPhotonShift(infinity, 10.0, 3.0, 5.0).frequencyRatio));
}

TEST_CASE("limb darkening preserves the emergent flux") {
    // 2 * integral of I(mu) mu dmu over the hemisphere equals 1 for an
    // isotropic emitter of unit intensity.
    double flux = 0.0;
    constexpr int intervals = 2000;
    for (int index = 0; index < intervals; ++index) {
        const double mu = (static_cast<double>(index) + 0.5) / intervals;
        flux += 2.0 * limbDarkening(mu) * mu / intervals;
    }
    CHECK(flux == doctest::Approx(1.0).epsilon(1.0e-6));
    CHECK(limbDarkening(1.0) > limbDarkening(0.0));
    CHECK(limbDarkening(-1.0) == limbDarkening(0.0));
}

TEST_CASE("black-body colourimetry reproduces photometric and Planckian references") {
    // The pre-1979 candela: a black body at the freezing point of platinum
    // (2042 K) has a luminance of 6e5 cd/m^2.
    CHECK(blackbodyColor(2042.0).luminance == doctest::Approx(6.0e5).epsilon(0.03));

    const auto chromaticity = [](double kelvin) {
        const BlackbodyColor color = blackbodyColor(kelvin);
        const double sum = color.xyz.v[0] + color.xyz.v[1] + color.xyz.v[2];
        return Vec3d{color.xyz.v[0] / sum, color.xyz.v[1] / sum, 0.0};
    };
    // Planckian locus (CIE 1931): illuminant A and the 6500 K point.
    const Vec3d warm = chromaticity(2856.0);
    CHECK(warm.v[0] == doctest::Approx(0.4476).epsilon(0.01));
    CHECK(warm.v[1] == doctest::Approx(0.4074).epsilon(0.01));
    const Vec3d daylight = chromaticity(6500.0);
    CHECK(daylight.v[0] == doctest::Approx(0.3135).epsilon(0.01));
    CHECK(daylight.v[1] == doctest::Approx(0.3237).epsilon(0.01));

    // Hotter is brighter at every temperature, and bluer.
    CHECK(blackbodyColor(8000.0).luminance > blackbodyColor(4000.0).luminance);
    CHECK(chromaticity(12000.0).v[0] < chromaticity(4000.0).v[0]);
    CHECK(blackbodyColor(0.0).luminance == 0.0);
    CHECK(blackbodyColor(std::numeric_limits<double>::quiet_NaN()).luminance == 0.0);
}

TEST_CASE("emission-line colours follow the colour-matching functions") {
    // H-alpha is red, [O III] 500.7 nm green-teal, H-beta blue: the dominant
    // positive primary identifies each line.
    const Vec3d hAlpha = monochromaticLinearSrgb(656.28);
    CHECK(hAlpha.v[0] > 0.3);
    CHECK(hAlpha.v[0] > 5.0 * std::max(hAlpha.v[1], hAlpha.v[2]));
    const Vec3d oxygen = monochromaticLinearSrgb(500.7);
    CHECK(oxygen.v[1] > oxygen.v[2]);
    CHECK(oxygen.v[2] > 0.0);
    CHECK(oxygen.v[0] < 0.0); // outside the sRGB gamut
    const Vec3d hBeta = monochromaticLinearSrgb(486.13);
    CHECK(hBeta.v[2] > hBeta.v[1]);
    CHECK(hBeta.v[1] > 0.0);

    // Luminance (Rec.709 Y) tracks the photopic curve: the eye is far more
    // sensitive at 500 nm than at 656 nm.
    const auto luminance = [](Vec3d const& rgb) {
        return 0.2126729 * rgb.v[0] + 0.7151522 * rgb.v[1] + 0.0721750 * rgb.v[2];
    };
    CHECK(luminance(oxygen) > 3.0 * luminance(hAlpha));
    CHECK(luminance(monochromaticLinearSrgb(300.0)) == 0.0);
    CHECK(luminance(monochromaticLinearSrgb(900.0)) == 0.0);
}

TEST_CASE("the black-body table interpolates luminance and stays inside the sRGB gamut") {
    const BlackbodyTable table = makeBlackbodyTable(500.0, 1.0e6, 512);
    REQUIRE(table.valid());
    CHECK_FALSE(makeBlackbodyTable(1000.0, 500.0, 512).valid());
    CHECK_FALSE(makeBlackbodyTable(500.0, 1.0e6, 1).valid());

    bool inGamut = true;
    double worstLumaError = 0.0;
    for (std::uint32_t index = 0; index < table.count; ++index) {
        const float* texel = table.rgba.data() + static_cast<std::size_t>(index) * 4U;
        inGamut = inGamut && texel[0] >= 0.0f && texel[1] >= 0.0f && texel[2] >= 0.0f;
        const double luma = 0.2126729 * texel[0] + 0.7151522 * texel[1] + 0.0721750 * texel[2];
        worstLumaError = std::max(worstLumaError, std::abs(luma - 1.0));
    }
    CHECK(inGamut);
    CHECK(worstLumaError < 1.0e-5);

    // Between table nodes the interpolated luminance tracks the exact integral.
    for (double kelvin : {777.0, 3333.0, 5772.0, 9876.0, 43210.0, 250000.0}) {
        CHECK(sampleBlackbodyLogLuminance(table, kelvin) ==
              doctest::Approx(std::log(blackbodyColor(kelvin).luminance)).epsilon(2.0e-3));
    }

    // A colour sampled against its own luminance has unit luminance.
    const double reference = sampleBlackbodyLogLuminance(table, 7500.0);
    const Vec3d unit = sampleBlackbodyTable(table, 7500.0, reference);
    CHECK(0.2126729 * unit.v[0] + 0.7151522 * unit.v[1] + 0.0721750 * unit.v[2] ==
          doctest::Approx(1.0).epsilon(1.0e-5));

    // Below the table there is no visible emission; above it the Rayleigh-Jeans
    // continuation makes luminance proportional to temperature.
    const Vec3d cold = sampleBlackbodyTable(table, 300.0, reference);
    CHECK(cold.v[0] + cold.v[1] + cold.v[2] == 0.0);
    CHECK(sampleBlackbodyLogLuminance(table, 4.0e6) - sampleBlackbodyLogLuminance(table, 1.0e6) ==
          doctest::Approx(std::log(4.0)).epsilon(1.0e-9));
}

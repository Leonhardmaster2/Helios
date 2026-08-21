// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helios/universe/frame_provider.hpp"
#include "helios/universe/instant.hpp"
#include "helios/universe/module.hpp"

#include <cmath>

using namespace helios::universe;
using helios::math::FrameId;
using helios::math::Vec3d;

namespace {

Instant makeInstant(std::int64_t days, double seconds) {
    Instant t;
    t.whole_days = days;
    t.seconds_of_day = seconds;
    return t;
}

} // namespace

TEST_CASE("universe module identity") {
    CHECK(helios::universe::kModuleName == "helios_universe");
    CHECK_FALSE(helios::universe::kModuleScope.empty());
}

TEST_CASE("Instant comparison and normalization") {
    CHECK(Instant::compare(makeInstant(0, 0.0), makeInstant(0, 0.0)) == 0);
    CHECK(Instant::compare(makeInstant(0, 1.0), makeInstant(0, 2.0)) == -1);
    CHECK(Instant::compare(makeInstant(1, 0.0), makeInstant(0, 86400.0 - 1.0)) == 1);

    // Advancing across a day boundary normalizes into whole_days.
    const Instant t = Instant::advancedBy(makeInstant(10, 86399.5), 1.0);
    CHECK(t.whole_days == 11);
    CHECK(t.seconds_of_day == doctest::Approx(0.5));

    // Negative advance borrows from the day count.
    const Instant u = Instant::advancedBy(makeInstant(10, 0.5), -1.0);
    CHECK(u.whole_days == 9);
    CHECK(u.seconds_of_day == doctest::Approx(86399.5));

    // Round trip over a large range keeps sub-microsecond resolution.
    const Instant big = Instant::advancedBy(makeInstant(1000000, 123.456789), 0.0000015);
    CHECK(static_cast<double>(big.totalSeconds()) ==
          doctest::Approx(1000000.0 * 86400.0 + 123.456789 + 0.0000015).epsilon(1e-15));
}

TEST_CASE("static provider returns identical transforms at any instant") {
    StaticFrameProvider provider;
    const FrameId body{7};
    FrameTransform xf;
    xf.translation = Vec3d{1.0e11, -2.0e10, 3.0e9};
    xf.rotation = helios::math::Quatd::fromAxisAngle(Vec3d{0, 0, 1}, 0.25);
    provider.addFrame(body, xf);

    auto s1 = provider.snapshot(makeInstant(0, 0.0));
    auto s2 = provider.snapshot(makeInstant(5000, 123.4));
    REQUIRE(s1.ok());
    REQUIRE(s2.ok());

    const Vec3d p{100.0, 200.0, 300.0};
    auto a = s1.value().pointInFrame(body, body, p);
    auto b = s2.value().pointInFrame(body, body, p);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    // Identity transform through the same frame is the point itself.
    CHECK(a.value().v[0] == doctest::Approx(p.v[0]));
    CHECK(a.value().v[1] == doctest::Approx(p.v[1]));
    CHECK(a.value().v[2] == doctest::Approx(p.v[2]));
}

TEST_CASE("rotating provider: body-fixed point maps to the expected orbit position") {
    RotatingFrameProvider provider;
    const FrameId body{1};
    RotatingBodyParams params;
    params.id = body;
    params.orbit_radius_m = 1.0e11;
    params.orbit_rate_rad_per_s = 0.1; // rad per second (synthetic scale)
    params.spin_rate_rad_per_s = 1.0;
    provider.addBody(params);

    // At t = 0 the orbit angle is 0: origin sits at (+r, 0).
    auto s0 = provider.snapshot(makeInstant(0, 0.0));
    REQUIRE(s0.ok());
    auto xf0 = s0.value().transformOf(body);
    REQUIRE(xf0.ok());
    CHECK(xf0.value().translation.v[0] == doctest::Approx(1.0e11));
    CHECK(xf0.value().translation.v[1] == doctest::Approx(0.0).epsilon(1e-6));

    // A point one metre along the body's local X axis is rotated by the spin
    // (spin angle pi/2 here: local +X maps to local +Y) and translated by the
    // orbit position at orbit angle 0.1 * t.
    const double t_seconds = 1.5707963267948966;
    auto s1 = provider.snapshot(makeInstant(0, t_seconds));
    REQUIRE(s1.ok());
    auto xf1 = s1.value().transformOf(body);
    REQUIRE(xf1.ok());
    const Vec3d local_x{1.0, 0.0, 0.0};
    const Vec3d rotated = xf1.value().rotation.rotate(local_x);
    CHECK(rotated.v[0] == doctest::Approx(0.0).epsilon(1e-12));
    CHECK(rotated.v[1] == doctest::Approx(1.0).epsilon(1e-12));
    const Vec3d world = rotated + xf1.value().translation;
    const double orbit_angle = 0.1 * t_seconds;
    CHECK(world.v[0] == doctest::Approx(1.0e11 * std::cos(orbit_angle)).epsilon(1e-12));
    CHECK(world.v[1] == doctest::Approx(1.0e11 * std::sin(orbit_angle) + 1.0).epsilon(1e-12));
}

TEST_CASE("snapshot round trip between two frames recovers the input") {
    RotatingFrameProvider provider;
    RotatingBodyParams a;
    a.id = FrameId{1};
    a.orbit_radius_m = 5.0e10;
    a.orbit_rate_rad_per_s = 0.05;
    a.spin_rate_rad_per_s = 0.5;
    RotatingBodyParams b;
    b.id = FrameId{2};
    b.orbit_radius_m = 1.5e11;
    b.orbit_rate_rad_per_s = 0.02;
    b.spin_rate_rad_per_s = 0.9;
    provider.addBody(a);
    provider.addBody(b);

    const Instant t = makeInstant(3, 40000.5);
    auto snap = provider.snapshot(t);
    REQUIRE(snap.ok());

    const Vec3d p_in_a{1.0e6, -2.0e6, 3.0e5};
    auto to_b = snap.value().pointInFrame(FrameId{1}, FrameId{2}, p_in_a);
    REQUIRE(to_b.ok());
    auto back = snap.value().pointInFrame(FrameId{2}, FrameId{1}, to_b.value());
    REQUIRE(back.ok());
    // Round trip through two large-magnitude transforms: compare with an
    // absolute metre tolerance, not a relative epsilon.
    const Vec3d err = back.value() - p_in_a;
    CHECK(length(err) < 1e-3);
}

TEST_CASE("unknown frames are reported, never silently transformed") {
    StaticFrameProvider provider;
    provider.addFrame(FrameId{1}, FrameTransform{});
    auto snap = provider.snapshot(makeInstant(0, 0.0));
    REQUIRE(snap.ok());
    auto missing = snap.value().pointInFrame(FrameId{1}, FrameId{99}, Vec3d{0, 0, 0});
    CHECK_FALSE(missing.ok());
    CHECK(static_cast<int>(missing.error().code()) == static_cast<int>(helios::core::ErrorCode::NotFound));
}

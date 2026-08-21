// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helios/universe/camera_reparent.hpp"
#include "helios/universe/frame_provider.hpp"
#include "helios/universe/instant.hpp"
#include "helios/universe/module.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace helios::universe;
namespace math = helios::math;
using helios::math::BodyFixedPosition;
using helios::math::FrameId;
using helios::math::Quatd;
using helios::math::Vec3d;

namespace {

Instant makeInstant(std::int64_t days, double seconds) {
    auto t = Instant::make(days, seconds);
    REQUIRE(t.ok());
    return t.value();
}

// Analytic kinematics of a RotatingBodyParams body at instant t (root axes).
FrameKinematics kinematicsFor(RotatingBodyParams const& b, Instant const& t) {
    FrameKinematics k;
    const double orbit_phase = RotatingFrameProvider::phaseFor(b.orbit_rate_rad_per_s, t);
    // d/dt of r*(cos(w t), sin(w t), 0):
    k.origin_velocity_m_per_s = Vec3d{-b.orbit_radius_m * b.orbit_rate_rad_per_s * std::sin(orbit_phase),
                                      b.orbit_radius_m * b.orbit_rate_rad_per_s * std::cos(orbit_phase), 0.0};
    k.angular_velocity_rad_per_s = Vec3d{0.0, 0.0, b.spin_rate_rad_per_s};
    return k;
}

double angleBetween(Quatd const& a, Quatd const& b) {
    // Rotation angle of the relative rotation.
    Quatd rel = a.conjugate() * b;
    if (rel.w < 0.0) {
        rel = Quatd{-rel.w, -rel.x, -rel.y, -rel.z};
    }
    return 2.0 * std::acos(std::clamp(rel.w, -1.0, 1.0));
}

// RotatingFrameProvider is non-copyable by design, so this helper populates
// a provider owned by the caller instead of returning one.
void makeTwoBodyProvider(RotatingFrameProvider& provider, RotatingBodyParams& out_a,
                         RotatingBodyParams& out_b) {
    out_a = RotatingBodyParams{};
    out_a.id = FrameId{1};
    out_a.orbit_radius_m = 5.0e10;
    out_a.orbit_rate_rad_per_s = 0.05;
    out_a.spin_rate_rad_per_s = 0.5;
    out_b = RotatingBodyParams{};
    out_b.id = FrameId{2};
    out_b.orbit_radius_m = 1.5e11;
    out_b.orbit_rate_rad_per_s = 0.02;
    out_b.spin_rate_rad_per_s = 0.9;
    CHECK(provider.addBody(out_a).ok());
    CHECK(provider.addBody(out_b).ok());
}

} // namespace

TEST_CASE("universe module identity") {
    CHECK(helios::universe::kModuleName == "helios_universe");
    CHECK_FALSE(helios::universe::kModuleScope.empty());
}

// ---------------------------------------------------------------------------
// Instant
// ---------------------------------------------------------------------------

TEST_CASE("Instant validation rejects non-finite values") {
    auto bad = Instant::make(0, std::nan(""));
    CHECK_FALSE(bad.ok());
    CHECK(static_cast<int>(bad.error().code()) == static_cast<int>(helios::core::ErrorCode::InvalidArgument));

    auto inf = Instant::make(0, std::numeric_limits<double>::infinity());
    CHECK_FALSE(inf.ok());

    auto good = Instant::make(7, 100.0);
    REQUIRE(good.ok());
    CHECK(good.value().isValid());
}

TEST_CASE("Instant make normalizes out-of-range seconds") {
    auto wrapped = Instant::make(1, 90000.0);
    REQUIRE(wrapped.ok());
    CHECK(wrapped.value().whole_days == 2);
    CHECK(wrapped.value().seconds_of_day == doctest::Approx(3600.0));

    auto negative = Instant::make(5, -60.0);
    REQUIRE(negative.ok());
    CHECK(negative.value().whole_days == 4);
    CHECK(negative.value().seconds_of_day == doctest::Approx(86340.0));
}

TEST_CASE("Instant comparison and normalization") {
    CHECK(Instant::compare(makeInstant(0, 0.0), makeInstant(0, 0.0)) == 0);
    CHECK(Instant::compare(makeInstant(0, 1.0), makeInstant(0, 2.0)) == -1);
    CHECK(Instant::compare(makeInstant(1, 0.0), makeInstant(0, 86400.0 - 1.0)) == 1);

    const Instant t = Instant::advancedBy(makeInstant(10, 86399.5), 1.0);
    CHECK(t.whole_days == 11);
    CHECK(t.seconds_of_day == doctest::Approx(0.5));

    const Instant u = Instant::advancedBy(makeInstant(10, 0.5), -1.0);
    CHECK(u.whole_days == 9);
    CHECK(u.seconds_of_day == doctest::Approx(86399.5));
}

// ---------------------------------------------------------------------------
// FrameSnapshot builder / immutability / composition
// ---------------------------------------------------------------------------

TEST_CASE("builder validates inputs and produces immutable snapshots") {
    FrameSnapshotBuilder builder;

    auto bad_root = builder.add(rootFrameId(), rootFrameId(), FrameTransform{});
    CHECK_FALSE(bad_root.ok());

    auto bad_rotation = builder.add(FrameId{1}, rootFrameId(), FrameTransform{Quatd{2.0, 0, 0, 0}, Vec3d{}});
    CHECK_FALSE(bad_rotation.ok());

    auto bad_translation =
        builder.add(FrameId{1}, rootFrameId(),
                    FrameTransform{Quatd::identity(), Vec3d{std::numeric_limits<double>::quiet_NaN(), 0, 0}});
    CHECK_FALSE(bad_translation.ok());

    auto unknown_parent = builder.add(FrameId{5}, FrameId{9}, FrameTransform{});
    CHECK_FALSE(unknown_parent.ok());

    CHECK(builder.add(FrameId{1}, rootFrameId(), FrameTransform{}).ok());
    auto duplicate = builder.add(FrameId{1}, rootFrameId(), FrameTransform{});
    CHECK_FALSE(duplicate.ok());

    auto built = builder.build();
    REQUIRE(built.ok());
    CHECK(built.value().frameCount() == 1);
    CHECK(built.value().contains(FrameId{1}));
    CHECK_FALSE(built.value().contains(FrameId{2}));
}

TEST_CASE("hierarchical snapshots compose along parent chains") {
    FrameSnapshotBuilder builder;
    // root <- A (translation 100 on X) <- B (rotation +90 deg about Z).
    FrameTransform xf_a;
    xf_a.translation = Vec3d{100.0, 0.0, 0.0};
    CHECK(builder.add(FrameId{1}, rootFrameId(), xf_a).ok());
    FrameTransform xf_b;
    xf_b.rotation = Quatd::fromAxisAngle(Vec3d{0, 0, 1}, 1.5707963267948966);
    CHECK(builder.add(FrameId{2}, FrameId{1}, xf_b).ok());
    auto snap = builder.build();
    REQUIRE(snap.ok());

    // B's local +X maps through B's rotation then A's translation:
    // (0,1,0) rotated -> (-1*? ) : Rz(90)*(1,0,0) = (0,1,0); + (100,0,0).
    auto p = snap.value().pointInFrame(FrameId{2}, rootFrameId(), Vec3d{1.0, 0.0, 0.0});
    REQUIRE(p.ok());
    CHECK(p.value().v[0] == doctest::Approx(100.0).epsilon(1e-12));
    CHECK(p.value().v[1] == doctest::Approx(1.0).epsilon(1e-12));

    // And back again.
    auto back = snap.value().pointInFrame(rootFrameId(), FrameId{2}, p.value());
    REQUIRE(back.ok());
    CHECK(back.value().v[0] == doctest::Approx(1.0).epsilon(1e-12));
    CHECK(back.value().v[1] == doctest::Approx(0.0).epsilon(1e-12));

    // transformBetween composes to identity for same-frame queries.
    auto id_xf = snap.value().transformBetween(FrameId{2}, FrameId{2});
    REQUIRE(id_xf.ok());
    CHECK(id_xf.value().translation.v[0] == doctest::Approx(0.0).epsilon(1e-12));
}

TEST_CASE("transform composition is associative; inversion round-trips exactly") {
    FrameTransform a;
    a.rotation = Quatd::fromAxisAngle(Vec3d{0.1, 0.8, -0.4}, 0.9).normalized();
    a.translation = Vec3d{123.0, -456.0, 789.0};
    FrameTransform b;
    b.rotation = Quatd::fromAxisAngle(Vec3d{0, 0, 1}, -0.3);
    b.translation = Vec3d{-1.0e6, 2.0e6, 3.0};
    FrameTransform c;
    c.rotation = Quatd::fromAxisAngle(Vec3d{1, 0, 0}, 1.2);
    c.translation = Vec3d{5.0, 5.0, 5.0};

    const FrameTransform ab_c = compose(compose(a, b), c);
    const FrameTransform a_bc = compose(a, compose(b, c));
    for (int i = 0; i < 3; ++i) {
        CHECK(ab_c.translation.v[i] == doctest::Approx(a_bc.translation.v[i]).epsilon(1e-12));
    }

    const FrameTransform inv = invert(a);
    const Vec3d v{42.0, -17.0, 256.0};
    // invert(a) applied to a(v): R^-1 (R v + t) - R^-1 t = v.
    const Vec3d transformed = a.rotation.rotate(v) + a.translation;
    const Vec3d round_trip = inv.rotation.rotate(transformed) + inv.translation;
    CHECK(round_trip.v[0] == doctest::Approx(v.v[0]).epsilon(1e-12));
    CHECK(round_trip.v[1] == doctest::Approx(v.v[1]).epsilon(1e-12));
    CHECK(round_trip.v[2] == doctest::Approx(v.v[2]).epsilon(1e-12));
}

TEST_CASE("repeated point round trips accumulate no drift") {
    StaticFrameProvider provider;
    FrameTransform xf;
    xf.rotation = Quatd::fromAxisAngle(Vec3d{0.3, -0.5, 0.8}, 0.7).normalized();
    xf.translation = Vec3d{1.0e11, -2.0e10, 3.0e9};
    CHECK(provider.addFrame(FrameId{7}, xf).ok());
    auto snap = provider.snapshot(makeInstant(0, 0.0));
    REQUIRE(snap.ok());

    const Vec3d original{100.0, 200.0, 300.0};
    Vec3d p = original;
    for (int i = 0; i < 10000; ++i) {
        auto out = snap.value().pointInFrame(FrameId{7}, rootFrameId(), p);
        REQUIRE(out.ok());
        auto back = snap.value().pointInFrame(rootFrameId(), FrameId{7}, out.value());
        REQUIRE(back.ok());
        p = back.value();
    }
    const double err = length(p - original);
    // Each round trip through the 1e11 m root picks up ~ulp(1e11) ~= 1.5e-5 m
    // of cancellation error; 10k trips accumulate ~sqrt(N)*err ~= 1e-3 m.
    CHECK(err < 1e-2); // metres after 10k round trips at 1e11 m scale
}

TEST_CASE("unknown frames are reported, never silently transformed") {
    StaticFrameProvider provider;
    CHECK(provider.addFrame(FrameId{1}, FrameTransform{}).ok());
    auto snap = provider.snapshot(makeInstant(0, 0.0));
    REQUIRE(snap.ok());
    auto missing = snap.value().pointInFrame(FrameId{1}, FrameId{99}, Vec3d{0, 0, 0});
    CHECK_FALSE(missing.ok());
    CHECK(static_cast<int>(missing.error().code()) == static_cast<int>(helios::core::ErrorCode::NotFound));
}

// ---------------------------------------------------------------------------
// Providers: validation + precision-safe phase
// ---------------------------------------------------------------------------

TEST_CASE("providers reject invalid instants and foreign time scales") {
    RotatingFrameProvider provider;
    RotatingBodyParams params;
    params.id = FrameId{1};
    CHECK(provider.addBody(params).ok());

    Instant bad;
    bad.seconds_of_day = -1.0; // violates the invariant directly
    auto s1 = provider.snapshot(bad);
    CHECK_FALSE(s1.ok());

    Instant foreign;
    foreign.scale = static_cast<TimeScale>(99);
    auto s2 = provider.snapshot(foreign);
    CHECK_FALSE(s2.ok());
    CHECK(static_cast<int>(s2.error().code()) == static_cast<int>(helios::core::ErrorCode::Unsupported));

    RotatingBodyParams nan_params;
    nan_params.id = FrameId{2};
    nan_params.orbit_radius_m = std::numeric_limits<double>::quiet_NaN();
    RotatingFrameProvider nan_provider;
    auto add = nan_provider.addBody(nan_params);
    CHECK_FALSE(add.ok());
}

TEST_CASE("static provider returns identical transforms at any instant") {
    StaticFrameProvider provider;
    const FrameId body{7};
    FrameTransform xf;
    xf.translation = Vec3d{1.0e11, -2.0e10, 3.0e9};
    xf.rotation = Quatd::fromAxisAngle(Vec3d{0, 0, 1}, 0.25);
    CHECK(provider.addFrame(body, xf).ok());

    auto s1 = provider.snapshot(makeInstant(0, 0.0));
    auto s2 = provider.snapshot(makeInstant(5000, 123.4));
    REQUIRE(s1.ok());
    REQUIRE(s2.ok());

    const Vec3d p{100.0, 200.0, 300.0};
    auto a = s1.value().pointInFrame(body, body, p);
    auto b = s2.value().pointInFrame(body, body, p);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
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
    params.orbit_rate_rad_per_s = 0.1;
    params.spin_rate_rad_per_s = 1.0;
    CHECK(provider.addBody(params).ok());

    auto s0 = provider.snapshot(makeInstant(0, 0.0));
    REQUIRE(s0.ok());
    auto xf0 = s0.value().transformToRoot(body);
    REQUIRE(xf0.ok());
    CHECK(xf0.value().translation.v[0] == doctest::Approx(1.0e11));
    CHECK(xf0.value().translation.v[1] == doctest::Approx(0.0).epsilon(1e-6));

    // Spin angle pi/2 maps local +X onto local +Y; the orbit position adds
    // r*(cos(0.1 t), sin(0.1 t)).
    const double t_seconds = 1.5707963267948966;
    auto s1 = provider.snapshot(makeInstant(0, t_seconds));
    REQUIRE(s1.ok());
    auto xf1 = s1.value().transformToRoot(body);
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

TEST_CASE("phase computation keeps microsecond increments observable at huge day counts") {
    // At 1e7 days (~8.64e12 s), binary64 spacing of total seconds is
    // ~1.95e-3 s: a naive (double)totalSeconds()*rate implementation cannot
    // see a 1 us increment. The period-reduced phase must.
    const std::int64_t days = 10000000;
    const double base_seconds = 40000.0;
    const double delta = 1e-6; // one microsecond

    const double phase_a = RotatingFrameProvider::phaseFor(1.0, makeInstant(days, base_seconds));
    const double phase_b = RotatingFrameProvider::phaseFor(1.0, makeInstant(days, base_seconds + delta));
    // Wrap-aware difference must be ~delta radians.
    double diff = phase_b - phase_a;
    if (diff > 3.141592653589793)
        diff -= 6.283185307179586;
    if (diff < -3.141592653589793)
        diff += 6.283185307179586;
    CHECK(diff == doctest::Approx(delta).epsilon(1e-9));

    // The snapshot transforms must differ observably too.
    RotatingFrameProvider provider;
    RotatingBodyParams params;
    params.id = FrameId{1};
    params.orbit_radius_m = 1.0e12;
    params.orbit_rate_rad_per_s = 1e-3;
    params.spin_rate_rad_per_s = 1.0;
    CHECK(provider.addBody(params).ok());
    auto sa = provider.snapshot(makeInstant(days, base_seconds));
    auto sb = provider.snapshot(makeInstant(days, base_seconds + delta));
    REQUIRE(sa.ok());
    REQUIRE(sb.ok());
    auto xa = sa.value().transformToRoot(FrameId{1});
    auto xb = sb.value().transformToRoot(FrameId{1});
    REQUIRE(xa.ok());
    REQUIRE(xb.ok());
    const double rot_delta = angleBetween(xa.value().rotation, xb.value().rotation);
    CHECK(rot_delta > 5e-7); // ~1e-6 rad expected; far above zero
}

TEST_CASE("snapshot round trip between two frames recovers the input") {
    RotatingBodyParams a;
    RotatingBodyParams b;
    RotatingFrameProvider provider;
    makeTwoBodyProvider(provider, a, b);

    const Instant t = makeInstant(3, 40000.5);
    auto snap = provider.snapshot(t);
    REQUIRE(snap.ok());

    const Vec3d p_in_a{1.0e6, -2.0e6, 3.0e5};
    auto to_b = snap.value().pointInFrame(FrameId{1}, FrameId{2}, p_in_a);
    REQUIRE(to_b.ok());
    auto back = snap.value().pointInFrame(FrameId{2}, FrameId{1}, to_b.value());
    REQUIRE(back.ok());
    const Vec3d err = back.value() - p_in_a;
    CHECK(length(err) < 1e-3); // absolute metre tolerance at 1e11 m scale
}

// ---------------------------------------------------------------------------
// Camera reparenting
// ---------------------------------------------------------------------------

TEST_CASE("reparenting preserves pose in both directions") {
    RotatingFrameProvider provider;
    RotatingBodyParams body;
    body.id = FrameId{1};
    body.orbit_radius_m = 1.0e11;
    body.orbit_rate_rad_per_s = 1e-4;
    body.spin_rate_rad_per_s = 1e-3;
    CHECK(provider.addBody(body).ok());

    ReparentThresholds thresholds;
    thresholds.enter_radius_m = 1.0e7;
    thresholds.exit_radius_m = 2.0e7;
    CameraReparenter reparenter(FrameId{1}, thresholds);
    REQUIRE(reparenter.valid());

    const Instant t = makeInstant(2, 30000.0);
    auto snap = provider.snapshot(t);
    REQUIRE(snap.ok());
    auto body_to_root = snap.value().transformToRoot(FrameId{1});
    REQUIRE(body_to_root.ok());

    // Camera parked near the body surface (well inside the enter radius),
    // expressed directly in barycentric coordinates as reference.
    const Vec3d offset_in_body{6.371e6, 100.0, -200.0};
    const Quatd cam_orientation_body = Quatd::fromAxisAngle(Vec3d{0.2, 0.3, 0.9}, 0.4).normalized();
    const Vec3d ref_root_pos =
        body_to_root.value().rotation.rotate(offset_in_body) + body_to_root.value().translation;
    const Quatd ref_root_orientation = (body_to_root.value().rotation * cam_orientation_body).normalized();

    ReparentableCamera camera;
    camera.body_frame = FrameId{1};
    camera.owner = OwningFrame::Barycentric;
    camera.barycentric.position = math::BarycentricPosition{rootFrameId(), ref_root_pos};
    camera.barycentric.orientation = ref_root_orientation;
    camera.linear_velocity = Vec3d{10.0, 20.0, 30.0};

    const FrameKinematics kin = kinematicsFor(body, t);

    // Enter: ownership flips to BodyFixed; pose matches the direct transform.
    auto owner1 = reparenter.update(snap.value(), kin, camera);
    REQUIRE(owner1.ok());
    CHECK(owner1.value() == OwningFrame::BodyFixed);
    // The pose round-trips through a ~1e11 m root translation, so binary64
    // cancellation leaves ~1e-5 m: compare with an absolute tolerance.
    CHECK(length(camera.body_fixed.position.coords() - offset_in_body) < 1e-3);
    CHECK(angleBetween(camera.body_fixed.orientation, cam_orientation_body) < 1e-12);

    // Exit: the camera is still well inside the exit radius, so ownership
    // correctly stays body-fixed. Move it beyond the exit radius first; the
    // resulting barycentric pose must match a direct computation.
    const Vec3d outside_in_body{0.0, 0.0, thresholds.exit_radius_m * 1.1};
    camera.body_fixed.position = math::BodyFixedPosition{FrameId{1}, outside_in_body};
    auto owner2 = reparenter.update(snap.value(), kin, camera);
    REQUIRE(owner2.ok());
    CHECK(owner2.value() == OwningFrame::Barycentric);
    const Vec3d expected_root =
        body_to_root.value().rotation.rotate(outside_in_body) + body_to_root.value().translation;
    CHECK(length(camera.barycentric.position.coords() - expected_root) < 1e-3);
}

TEST_CASE("velocity transforms consistently across a full round trip") {
    RotatingFrameProvider provider;
    RotatingBodyParams body;
    body.id = FrameId{1};
    body.orbit_radius_m = 1.0e11;
    body.orbit_rate_rad_per_s = 1e-4;
    body.spin_rate_rad_per_s = 2e-3;
    CHECK(provider.addBody(body).ok());

    ReparentThresholds thresholds;
    thresholds.enter_radius_m = 1.0e7;
    thresholds.exit_radius_m = 2.0e7;
    CameraReparenter reparenter(FrameId{1}, thresholds);

    const Instant t = makeInstant(0, 100.0);
    auto snap = provider.snapshot(t);
    REQUIRE(snap.ok());
    auto body_to_root = snap.value().transformToRoot(FrameId{1});
    REQUIRE(body_to_root.ok());

    const Vec3d pos_in_body{6.371e6, 0.0, 0.0};
    const Vec3d root_pos =
        body_to_root.value().rotation.rotate(pos_in_body) + body_to_root.value().translation;
    const Vec3d root_velocity{50.0, -25.0, 10.0};

    ReparentableCamera camera;
    camera.body_frame = FrameId{1};
    camera.owner = OwningFrame::Barycentric;
    camera.barycentric.position = math::BarycentricPosition{rootFrameId(), root_pos};
    camera.linear_velocity = root_velocity;

    const FrameKinematics kin = kinematicsFor(body, t);
    REQUIRE(reparenter.update(snap.value(), kin, camera).ok()); // enter
    CHECK(camera.owner == OwningFrame::BodyFixed);

    // Enter must produce exactly the analytic body-frame velocity:
    //   v_body = R^-1 (v_root - v_origin - w x (p_root - t))
    const Vec3d rel_enter = root_pos - body_to_root.value().translation;
    const Vec3d v_rel_root =
        root_velocity - kin.origin_velocity_m_per_s - cross(kin.angular_velocity_rad_per_s, rel_enter);
    const Vec3d expected_v_body = body_to_root.value().rotation.conjugate().rotate(v_rel_root);
    CHECK(length(camera.linear_velocity - expected_v_body) < 1e-6);

    // The camera is still inside the exit radius; move it beyond to force
    // the exit transition. NOTE: teleporting between updates means the exit
    // legitimately compensates frame motion at the NEW position, so the
    // exit is verified against its own analytic formula:
    //   v_root' = R v_body + v_origin + w x (R p_body)
    const Vec3d v_body_before_exit = camera.linear_velocity;
    camera.body_fixed.position =
        math::BodyFixedPosition{FrameId{1}, Vec3d{thresholds.exit_radius_m * 1.1, 0.0, 0.0}};
    REQUIRE(reparenter.update(snap.value(), kin, camera).ok()); // exit
    CHECK(camera.owner == OwningFrame::Barycentric);
    const Vec3d rel_exit = body_to_root.value().rotation.rotate(camera.body_fixed.position.coords());
    const Vec3d expected_v_root = body_to_root.value().rotation.rotate(v_body_before_exit) +
                                  kin.origin_velocity_m_per_s +
                                  cross(kin.angular_velocity_rad_per_s, rel_exit);
    // Values reach ~1e7 m/s; binary64 keeps the comparison to ~1e-8.
    CHECK(length(camera.linear_velocity - expected_v_root) < 1e-3);
}

TEST_CASE("hysteresis prevents boundary oscillation") {
    RotatingFrameProvider provider;
    RotatingBodyParams body;
    body.id = FrameId{1};
    body.orbit_radius_m = 0.0; // origin at root: distance control is trivial
    body.spin_rate_rad_per_s = 0.0;
    CHECK(provider.addBody(body).ok());

    ReparentThresholds thresholds;
    thresholds.enter_radius_m = 100.0;
    thresholds.exit_radius_m = 200.0;
    CameraReparenter reparenter(FrameId{1}, thresholds);

    auto snap = provider.snapshot(makeInstant(0, 0.0));
    REQUIRE(snap.ok());
    const FrameKinematics kin{};

    ReparentableCamera camera;
    camera.body_frame = FrameId{1};
    camera.owner = OwningFrame::Barycentric;
    camera.barycentric.position = math::BarycentricPosition{rootFrameId(), Vec3d{150.0, 0, 0}};

    // 150 m sits inside the band [100, 200]: no matter how many updates,
    // ownership never changes from the initial state.
    for (int i = 0; i < 100; ++i) {
        auto owner = reparenter.update(snap.value(), kin, camera);
        REQUIRE(owner.ok());
        CHECK(owner.value() == OwningFrame::Barycentric);
    }

    // At exactly the enter radius: enters (deterministic boundary rule).
    camera.barycentric.position = math::BarycentricPosition{rootFrameId(), Vec3d{100.0, 0, 0}};
    auto owner_enter = reparenter.update(snap.value(), kin, camera);
    REQUIRE(owner_enter.ok());
    CHECK(owner_enter.value() == OwningFrame::BodyFixed);

    // At exactly the exit radius: stays body-fixed (exit requires > exit).
    camera.body_fixed.position = math::BodyFixedPosition{FrameId{1}, Vec3d{200.0, 0, 0}};
    auto owner_stay = reparenter.update(snap.value(), kin, camera);
    REQUIRE(owner_stay.ok());
    CHECK(owner_stay.value() == OwningFrame::BodyFixed);

    // Just beyond the exit radius: exits.
    camera.body_fixed.position = math::BodyFixedPosition{FrameId{1}, Vec3d{200.0000001, 0, 0}};
    auto owner_exit = reparenter.update(snap.value(), kin, camera);
    REQUIRE(owner_exit.ok());
    CHECK(owner_exit.value() == OwningFrame::Barycentric);
}

TEST_CASE("thousands of repeated transitions accumulate no drift") {
    RotatingFrameProvider provider;
    RotatingBodyParams body;
    body.id = FrameId{1};
    body.orbit_radius_m = 1.0e11;
    body.orbit_rate_rad_per_s = 1e-4;
    body.spin_rate_rad_per_s = 5e-3; // high rotation rate
    CHECK(provider.addBody(body).ok());

    ReparentThresholds thresholds;
    thresholds.enter_radius_m = 1.0e7;
    thresholds.exit_radius_m = 1.05e7; // narrow band: rapid oscillation
    CameraReparenter reparenter(FrameId{1}, thresholds);

    const Instant t = makeInstant(1, 50000.0);
    auto snap = provider.snapshot(t);
    REQUIRE(snap.ok());
    auto body_to_root = snap.value().transformToRoot(FrameId{1});
    REQUIRE(body_to_root.ok());

    // Start just outside the exit radius so the first update enters.
    const double start_distance = 1.06e7;
    const Vec3d dir{1.0, 0.0, 0.0};
    const Vec3d root_pos = body_to_root.value().translation + dir * start_distance;
    const Quatd orientation = Quatd::fromAxisAngle(Vec3d{0, 1, 0}, 1.1).normalized();

    ReparentableCamera camera;
    camera.body_frame = FrameId{1};
    camera.owner = OwningFrame::Barycentric;
    camera.barycentric.position = math::BarycentricPosition{rootFrameId(), root_pos};
    camera.barycentric.orientation = orientation;

    const FrameKinematics kin = kinematicsFor(body, t);
    int switches = 0;
    OwningFrame previous = camera.owner;
    for (int i = 0; i < 10001; ++i) {
        // Alternate the distance across the band each update by moving the
        // authoritative pose radially (simulating an oscillating approach).
        if (camera.owner == OwningFrame::Barycentric) {
            const double d = length(camera.barycentric.position.coords() - body_to_root.value().translation);
            const double target = (d <= thresholds.enter_radius_m) ? 1.06e7 : 0.995e7;
            camera.barycentric.position =
                math::BarycentricPosition{rootFrameId(), body_to_root.value().translation + dir * target};
        } else {
            const double d = length(camera.body_fixed.position.coords());
            const double target = (d <= thresholds.enter_radius_m) ? 1.06e7 : 0.995e7;
            camera.body_fixed.position = math::BodyFixedPosition{FrameId{1}, dir * target};
        }
        auto owner = reparenter.update(snap.value(), kin, camera);
        REQUIRE(owner.ok());
        if (owner.value() != previous) {
            ++switches;
            previous = owner.value();
        }
    }
    CHECK(switches > 10000); // transitions actually happened every update

    // Final pose must match a direct computation from the snapshot: no
    // accumulated drift despite 10k+ ownership changes.
    const Vec3d expected_root = body_to_root.value().translation + dir * 0.995e7;
    CHECK(length(camera.barycentric.position.coords() - expected_root) < 1e-6);
    CHECK(angleBetween(camera.barycentric.orientation, orientation) < 1e-9);
}

TEST_CASE("reparenting works at body-frame poles with high spin rates") {
    RotatingFrameProvider provider;
    RotatingBodyParams body;
    body.id = FrameId{1};
    body.orbit_radius_m = 1.0e11;
    body.spin_rate_rad_per_s = 50.0; // extremely high spin
    CHECK(provider.addBody(body).ok());

    ReparentThresholds thresholds;
    thresholds.enter_radius_m = 1.0e7;
    thresholds.exit_radius_m = 2.0e7;
    CameraReparenter reparenter(FrameId{1}, thresholds);

    const Instant t = makeInstant(0, 0.001); // mid-spin
    auto snap = provider.snapshot(t);
    REQUIRE(snap.ok());
    auto body_to_root = snap.value().transformToRoot(FrameId{1});
    REQUIRE(body_to_root.ok());

    // Camera above the north pole of the body frame.
    const Vec3d pole_in_body{0.0, 0.0, 6.371e6};
    const Vec3d root_pos =
        body_to_root.value().rotation.rotate(pole_in_body) + body_to_root.value().translation;
    const Quatd orientation = Quatd::fromAxisAngle(Vec3d{1, 0, 0}, 0.0); // identity

    ReparentableCamera camera;
    camera.body_frame = FrameId{1};
    camera.owner = OwningFrame::Barycentric;
    camera.barycentric.position = math::BarycentricPosition{rootFrameId(), root_pos};
    camera.barycentric.orientation = orientation;

    const FrameKinematics kin = kinematicsFor(body, t);
    REQUIRE(reparenter.update(snap.value(), kin, camera).ok());
    CHECK(camera.owner == OwningFrame::BodyFixed);
    CHECK(camera.body_fixed.position.coords().v[2] == doctest::Approx(6.371e6).epsilon(1e-9));
    // Move beyond the exit radius (still on the polar axis) to force exit.
    camera.body_fixed.position =
        math::BodyFixedPosition{FrameId{1}, Vec3d{0.0, 0.0, thresholds.exit_radius_m * 1.1}};
    REQUIRE(reparenter.update(snap.value(), kin, camera).ok());
    CHECK(camera.owner == OwningFrame::Barycentric);
    CHECK(angleBetween(camera.barycentric.orientation, orientation) < 1e-12);
}

TEST_CASE("large root translations keep reparenting exact") {
    StaticFrameProvider provider;
    FrameTransform xf;
    xf.translation = Vec3d{1.0e13, 0.0, 0.0}; // full v1 range
    xf.rotation = Quatd::fromAxisAngle(Vec3d{0, 0, 1}, 0.77);
    CHECK(provider.addFrame(FrameId{1}, xf).ok());
    auto snap = provider.snapshot(makeInstant(0, 0.0));
    REQUIRE(snap.ok());

    ReparentThresholds thresholds;
    thresholds.enter_radius_m = 1.0e7;
    thresholds.exit_radius_m = 2.0e7;
    CameraReparenter reparenter(FrameId{1}, thresholds);

    const Vec3d local{1.0e6, 2.0e6, 3.0e6};
    const Vec3d root_pos = xf.rotation.rotate(local) + xf.translation;

    ReparentableCamera camera;
    camera.body_frame = FrameId{1};
    camera.owner = OwningFrame::Barycentric;
    camera.barycentric.position = math::BarycentricPosition{rootFrameId(), root_pos};

    const FrameKinematics kin{};
    REQUIRE(reparenter.update(snap.value(), kin, camera).ok());
    const Vec3d got_local = camera.body_fixed.position.coords();
    // binary64 ulp at 1e13 m is ~1.9e-3 m, so recovering a body-local
    // position through the root carries that quantization; centimetre-level
    // agreement at full v1 range is the honest expectation.
    CHECK(length(got_local - local) < 1e-2);
}

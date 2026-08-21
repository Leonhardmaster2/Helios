// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helios/render/scene_assembler.hpp"
#include "helios/universe/frame_provider.hpp"

#include <cmath>
#include <limits>

using namespace helios::render;
using namespace helios::universe;
namespace math = helios::math;
using helios::math::BarycentricPosition;
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

SceneAssemblerInput basicInput(Vec3d camera_pos) {
    SceneAssemblerInput in;
    in.storage_frame = rootFrameId();
    in.camera_position = camera_pos;
    in.max_near_distance_m = 1.0e9;
    return in;
}

RotatingBodyParams makeBody(FrameId id) {
    RotatingBodyParams p;
    p.id = id;
    p.orbit_radius_m = 1.0e11;
    p.orbit_rate_rad_per_s = 1e-3;
    p.spin_rate_rad_per_s = 1e-2;
    return p;
}

} // namespace

TEST_CASE("assembler rejects invalid construction and inputs") {
    auto bad = SceneAssembler::create(0.0);
    CHECK_FALSE(bad.ok());
    auto nan = SceneAssembler::create(std::numeric_limits<double>::quiet_NaN());
    CHECK_FALSE(nan.ok());
    auto good = SceneAssembler::create(1.0e9);
    REQUIRE(good.ok());

    auto snap_result = StaticFrameProvider{}.snapshot(makeInstant(0, 0.0));
    REQUIRE(snap_result.ok());

    auto in = basicInput(Vec3d{0, 0, 0});
    in.near_barycentric.push_back(
        BarycentricPosition{rootFrameId(), Vec3d{std::numeric_limits<double>::infinity(), 0, 0}});
    auto r = good.value().assemble(snap_result.value(), in);
    CHECK_FALSE(r.ok());
    CHECK(static_cast<int>(r.error().code()) == static_cast<int>(helios::core::ErrorCode::InvalidArgument));
}

TEST_CASE("assembler rejects a storage frame unknown to the snapshot") {
    auto assembler = SceneAssembler::create(1.0e9);
    REQUIRE(assembler.ok());
    auto snap_result = StaticFrameProvider{}.snapshot(makeInstant(0, 0.0));
    REQUIRE(snap_result.ok());

    auto in = basicInput(Vec3d{0, 0, 0});
    in.storage_frame = FrameId{42}; // not in the (empty) snapshot
    auto r = assembler.value().assemble(snap_result.value(), in);
    CHECK_FALSE(r.ok());
    CHECK(static_cast<int>(r.error().code()) == static_cast<int>(helios::core::ErrorCode::NotFound));
}

TEST_CASE("subtraction happens before the float cast at astronomical range") {
    auto assembler = SceneAssembler::create(1.0e9);
    REQUIRE(assembler.ok());
    auto snap_result = StaticFrameProvider{}.snapshot(makeInstant(0, 0.0));
    REQUIRE(snap_result.ok());

    // Camera near a landmark at 1e12 m from the root; a second landmark is
    // 1 mm farther out. A float32 cast BEFORE subtraction would collapse
    // both to the same value (binary32 spacing at 1e12 m is ~65536 m); the
    // double subtraction must keep them distinct after the cast.
    const double base = 1.0e12;
    auto in = basicInput(Vec3d{base - 50.0, 0.0, 0.0});
    in.near_barycentric.push_back(BarycentricPosition{rootFrameId(), Vec3d{base, 0.0, 0.0}});
    in.near_barycentric.push_back(BarycentricPosition{rootFrameId(), Vec3d{base + 0.001, 0.0, 0.0}});

    auto scene = assembler.value().assemble(snap_result.value(), in);
    REQUIRE(scene.ok());
    REQUIRE(scene.value().near_records.size() == 2);
    CHECK(scene.value().near_records[0].offset_m.v[0] == doctest::Approx(50.0).epsilon(1e-6));
    CHECK(scene.value().near_records[1].offset_m.v[0] == doctest::Approx(50.001).epsilon(1e-6));
    CHECK(scene.value().near_records[0].offset_m.v[0] != scene.value().near_records[1].offset_m.v[0]);
}

TEST_CASE("local precision survives a 1e13 m root position") {
    auto assembler = SceneAssembler::create(1.0e9);
    REQUIRE(assembler.ok());
    auto snap_result = StaticFrameProvider{}.snapshot(makeInstant(0, 0.0));
    REQUIRE(snap_result.ok());

    // Landmarks 100 m and 100.05 m from the camera at 1e13 m: the records
    // must reflect the local offsets, not the astronomical coordinate.
    // (binary64 ulp at 1e13 m is ~1.9e-3 m, so millimetre-level claims are
    // impossible there; a 5 cm separation is ~25 ulps and must survive.)
    const double base = 1.0e13;
    auto in = basicInput(Vec3d{base, 0.0, 0.0});
    in.near_barycentric.push_back(BarycentricPosition{rootFrameId(), Vec3d{base + 100.0, 0.0, 0.0}});
    in.near_barycentric.push_back(BarycentricPosition{rootFrameId(), Vec3d{base + 100.05, 0.0, 0.0}});
    auto scene = assembler.value().assemble(snap_result.value(), in);
    REQUIRE(scene.ok());
    REQUIRE(scene.value().near_records.size() == 2);
    CHECK(scene.value().near_records[0].offset_m.v[0] == doctest::Approx(100.0f).epsilon(1e-6));
    CHECK(std::abs(scene.value().near_records[1].offset_m.v[0] - 100.05) < 0.01);
}

TEST_CASE("near origins beyond the bound are rejected, not truncated") {
    auto assembler = SceneAssembler::create(1.0e9);
    REQUIRE(assembler.ok());
    auto snap_result = StaticFrameProvider{}.snapshot(makeInstant(0, 0.0));
    REQUIRE(snap_result.ok());

    auto in = basicInput(Vec3d{0.0, 0.0, 0.0});
    in.near_barycentric.push_back(BarycentricPosition{rootFrameId(), Vec3d{2.0e9, 0.0, 0.0}});
    auto r = assembler.value().assemble(snap_result.value(), in);
    CHECK_FALSE(r.ok());
    CHECK(static_cast<int>(r.error().code()) == static_cast<int>(helios::core::ErrorCode::OutOfRange));
}

TEST_CASE("distant bodies use direction/angular representation, never a float32 centre") {
    auto assembler = SceneAssembler::create(1.0e9);
    REQUIRE(assembler.ok());
    auto snap_result = StaticFrameProvider{}.snapshot(makeInstant(0, 0.0));
    REQUIRE(snap_result.ok());

    auto in = basicInput(Vec3d{0.0, 0.0, 0.0});
    DistantBodyInput body;
    body.center = BarycentricPosition{rootFrameId(), Vec3d{1.0e12, 0.0, 0.0}};
    body.radius_m = 6.371e6;
    in.distant_bodies.push_back(body);

    auto scene = assembler.value().assemble(snap_result.value(), in);
    REQUIRE(scene.ok());
    REQUIRE(scene.value().distant_records.size() == 1);
    CHECK(scene.value().distant_records[0].direction_unit.v[0] == doctest::Approx(1.0).epsilon(1e-15));
    CHECK(scene.value().distant_records[0].distance_m == doctest::Approx(1.0e12));
    CHECK(scene.value().distant_records[0].angular_radius_rad ==
          doctest::Approx(std::asin(6.371e6 / 1.0e12)).epsilon(1e-15));

    // Degenerate bodies are rejected.
    DistantBodyInput bad = body;
    bad.radius_m = -1.0;
    in.distant_bodies.clear();
    in.distant_bodies.push_back(bad);
    auto r = assembler.value().assemble(snap_result.value(), in);
    CHECK_FALSE(r.ok());
}

// ---------------------------------------------------------------------------
// Snapshot-backed frame support (P0): body-fixed inputs and body-fixed
// camera storage are transformed through the FrameSnapshot.
// ---------------------------------------------------------------------------

TEST_CASE("body-fixed origins are transformed through the snapshot") {
    RotatingFrameProvider provider;
    CHECK(provider.addBody(makeBody(FrameId{1})).ok());

    const Instant t = makeInstant(0, 12.5);
    auto snap_result = provider.snapshot(t);
    REQUIRE(snap_result.ok());
    auto body_to_root = snap_result.value().transformToRoot(FrameId{1});
    REQUIRE(body_to_root.ok());

    // Camera at the body origin (root storage); landmark on the surface.
    auto assembler = SceneAssembler::create(1.0e9);
    REQUIRE(assembler.ok());
    auto in = basicInput(body_to_root.value().translation);
    in.near_body_fixed.push_back(BodyFixedPosition{FrameId{1}, Vec3d{6.371e6, 0.0, 0.0}});

    auto scene = assembler.value().assemble(snap_result.value(), in);
    REQUIRE(scene.ok());
    REQUIRE(scene.value().near_records.size() == 1);

    // Expected: the body-fixed point transformed to root by the snapshot,
    // minus the camera position — i.e. the rotated surface vector. NOTE:
    // the record is binary32, so at 6.371e6 m magnitude the components are
    // quantized to 0.5 m; compare against the float32-cast expectation.
    const Vec3d expected = body_to_root.value().rotation.rotate(Vec3d{6.371e6, 0.0, 0.0});
    const Vec3d got{scene.value().near_records[0].offset_m.v[0], scene.value().near_records[0].offset_m.v[1],
                    scene.value().near_records[0].offset_m.v[2]};
    const math::Vec3f expected_f{static_cast<float>(expected.v[0]), static_cast<float>(expected.v[1]),
                                 static_cast<float>(expected.v[2])};
    CHECK(length(got - Vec3d{expected_f.v[0], expected_f.v[1], expected_f.v[2]}) < 0.01);

    // PROOF the snapshot is actually used: at a different instant, with the
    // camera following the body origin, the same body-fixed input MUST
    // assemble to a different offset (the body has rotated), and it must
    // match the manual snapshot computation there too.
    const Instant t2 = makeInstant(0, 12.5 + 100.0);
    auto snap2 = provider.snapshot(t2);
    REQUIRE(snap2.ok());
    auto body_to_root2 = snap2.value().transformToRoot(FrameId{1});
    REQUIRE(body_to_root2.ok());
    in.camera_position = body_to_root2.value().translation; // follow the body
    auto scene2 = assembler.value().assemble(snap2.value(), in);
    REQUIRE(scene2.ok());
    const Vec3d got2{scene2.value().near_records[0].offset_m.v[0],
                     scene2.value().near_records[0].offset_m.v[1],
                     scene2.value().near_records[0].offset_m.v[2]};
    CHECK(length(got - got2) > 1.0); // rotation actually moved it
    const Vec3d expected2 = body_to_root2.value().rotation.rotate(Vec3d{6.371e6, 0.0, 0.0});
    const math::Vec3f expected2_f{static_cast<float>(expected2.v[0]), static_cast<float>(expected2.v[1]),
                                  static_cast<float>(expected2.v[2])};
    CHECK(length(got2 - Vec3d{expected2_f.v[0], expected2_f.v[1], expected2_f.v[2]}) < 0.01);
}

TEST_CASE("body-fixed camera storage works through the snapshot") {
    RotatingFrameProvider provider;
    CHECK(provider.addBody(makeBody(FrameId{1})).ok());

    auto snap_result = provider.snapshot(makeInstant(0, 40.0));
    REQUIRE(snap_result.ok());
    auto body_to_root = snap_result.value().transformToRoot(FrameId{1});
    REQUIRE(body_to_root.ok());

    // Camera stored IN the body frame at (1000, 0, 0).
    auto assembler = SceneAssembler::create(1.0e9);
    REQUIRE(assembler.ok());
    SceneAssemblerInput in;
    in.storage_frame = FrameId{1};
    in.camera_position = Vec3d{1000.0, 0.0, 0.0};
    in.max_near_distance_m = 1.0e9;
    // Same-frame body-fixed origin at the body centre: the checked
    // difference is exactly -1000 X, no root round trip involved.
    in.near_body_fixed.push_back(BodyFixedPosition{FrameId{1}, Vec3d{0.0, 0.0, 0.0}});
    // A genuinely nearby ROOT-frame point: 50 m along the body's local X
    // ahead of the camera, expressed in barycentric coordinates. This
    // exercises the root -> storage-frame snapshot transform with a point
    // that is near in both frames.
    const Vec3d cam_in_root =
        body_to_root.value().rotation.rotate(in.camera_position) + body_to_root.value().translation;
    const Vec3d ahead_in_root =
        body_to_root.value().rotation.rotate(Vec3d{1050.0, 0.0, 0.0}) + body_to_root.value().translation;
    in.near_barycentric.push_back(BarycentricPosition{rootFrameId(), ahead_in_root});

    auto scene = assembler.value().assemble(snap_result.value(), in);
    REQUIRE(scene.ok());
    REQUIRE(scene.value().near_records.size() == 2);
    // Records are emitted barycentric-first, then body-fixed.

    // The nearby root point must assemble to exactly +50 X in the body
    // frame: the snapshot transformed it root -> body in double, then one
    // double subtraction, then the float cast. Y/Z pick up ~ulp(1e11) of
    // cancellation from the root-space round trip (~1.5e-5 m per hop).
    CHECK(scene.value().near_records[0].offset_m.v[0] == doctest::Approx(50.0f));
    CHECK(std::abs(scene.value().near_records[0].offset_m.v[1]) < 1e-3);
    CHECK(std::abs(scene.value().near_records[0].offset_m.v[2]) < 1e-3);

    // Same-frame body-fixed origin: exact in binary64 and exactly
    // representable in binary32.
    CHECK(scene.value().near_records[1].offset_m.v[0] == doctest::Approx(-1000.0f));
    CHECK(scene.value().near_records[1].offset_m.v[1] == doctest::Approx(0.0f));
    CHECK(scene.value().near_records[1].offset_m.v[2] == doctest::Approx(0.0f));

    // Sanity: the camera really is ~1e11 from the root origin, so a root
    // origin at (0,0,0) must be REJECTED as near and classify as distant.
    SceneAssemblerInput far_in = in;
    far_in.near_barycentric.clear();
    far_in.near_barycentric.push_back(BarycentricPosition{rootFrameId(), Vec3d{0.0, 0.0, 0.0}});
    auto far_result = assembler.value().assemble(snap_result.value(), far_in);
    CHECK_FALSE(far_result.ok());
    CHECK(static_cast<int>(far_result.error().code()) ==
          static_cast<int>(helios::core::ErrorCode::OutOfRange));
}

TEST_CASE("cross-frame body-fixed inputs from different frames both resolve") {
    RotatingFrameProvider provider;
    RotatingBodyParams a = makeBody(FrameId{1});
    RotatingBodyParams b = makeBody(FrameId{2});
    b.orbit_radius_m = 1.5e11;
    CHECK(provider.addBody(a).ok());
    CHECK(provider.addBody(b).ok());

    auto snap_result = provider.snapshot(makeInstant(1, 5000.0));
    REQUIRE(snap_result.ok());

    // Camera stored at body 1; landmark fixed to body 2's frame.
    auto assembler = SceneAssembler::create(1.0e9);
    REQUIRE(assembler.ok());
    SceneAssemblerInput in;
    in.storage_frame = FrameId{1};
    in.camera_position = Vec3d{0.0, 0.0, 0.0};
    in.max_near_distance_m = 1.0e9; // body 2 is ~1e11 away: beyond the bound
    in.near_body_fixed.push_back(BodyFixedPosition{FrameId{2}, Vec3d{0.0, 0.0, 0.0}});

    // Beyond the representation bound -> rejected (correct classification).
    auto r = assembler.value().assemble(snap_result.value(), in);
    CHECK_FALSE(r.ok());
    CHECK(static_cast<int>(r.error().code()) == static_cast<int>(helios::core::ErrorCode::OutOfRange));

    // As a DISTANT body-fixed centre it must resolve through the snapshot.
    in.near_body_fixed.clear();
    in.distant_body_fixed.push_back(
        DistantBodyInputFixed{BodyFixedPosition{FrameId{2}, Vec3d{0, 0, 0}}, 6.371e6});
    auto scene = assembler.value().assemble(snap_result.value(), in);
    REQUIRE(scene.ok());
    REQUIRE(scene.value().distant_records.size() == 1);
    // Both synthetic bodies share the orbit phase, so the separation is the
    // radial difference; verify against the snapshot directly.
    auto b_to_root = snap_result.value().transformToRoot(FrameId{2});
    REQUIRE(b_to_root.ok());
    auto a_to_root = snap_result.value().transformToRoot(FrameId{1});
    REQUIRE(a_to_root.ok());
    const double expected_dist = length(b_to_root.value().translation - a_to_root.value().translation);
    CHECK(scene.value().distant_records[0].distance_m == doctest::Approx(expected_dist).epsilon(1e-9));
}

TEST_CASE("near geometry works through rotating frame snapshots") {
    RotatingFrameProvider provider;
    CHECK(provider.addBody(makeBody(FrameId{1})).ok());

    const Instant t = makeInstant(0, 12.5);
    auto snap_result = provider.snapshot(t);
    REQUIRE(snap_result.ok());
    auto body_to_root = snap_result.value().transformToRoot(FrameId{1});
    REQUIRE(body_to_root.ok());

    // Camera 1000 m along the body's local X; landmark 1100 m along it.
    const Vec3d cam_local{1000.0, 0.0, 0.0};
    const Vec3d cam_root = body_to_root.value().rotation.rotate(cam_local) + body_to_root.value().translation;
    auto assembler = SceneAssembler::create(1.0e9);
    REQUIRE(assembler.ok());
    auto in = basicInput(cam_root);
    in.near_barycentric.push_back(
        BarycentricPosition{rootFrameId(), body_to_root.value().rotation.rotate(Vec3d{1100.0, 0.0, 0.0}) +
                                               body_to_root.value().translation});
    auto scene = assembler.value().assemble(snap_result.value(), in);
    REQUIRE(scene.ok());
    REQUIRE(scene.value().near_records.size() == 1);
    // Offsets are expressed in STORAGE-frame axes (here: barycentric), so
    // the body's spin rotates the offset vector; its magnitude is exact.
    const math::Vec3d offset{scene.value().near_records[0].offset_m.v[0],
                             scene.value().near_records[0].offset_m.v[1],
                             scene.value().near_records[0].offset_m.v[2]};
    CHECK(length(offset) == doctest::Approx(100.0).epsilon(1e-6));
}

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
    in.camera.position = BarycentricPosition{rootFrameId(), camera_pos};
    in.max_near_distance_m = 1.0e9;
    return in;
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
    in.near_origins.push_back(
        BarycentricPosition{rootFrameId(), Vec3d{std::numeric_limits<double>::infinity(), 0, 0}});
    auto r = good.value().assemble(snap_result.value(), in);
    CHECK_FALSE(r.ok());
    CHECK(static_cast<int>(r.error().code()) == static_cast<int>(helios::core::ErrorCode::InvalidArgument));
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
    in.near_origins.push_back(BarycentricPosition{rootFrameId(), Vec3d{base, 0.0, 0.0}});
    in.near_origins.push_back(BarycentricPosition{rootFrameId(), Vec3d{base + 0.001, 0.0, 0.0}});

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
    in.near_origins.push_back(BarycentricPosition{rootFrameId(), Vec3d{base + 100.0, 0.0, 0.0}});
    in.near_origins.push_back(BarycentricPosition{rootFrameId(), Vec3d{base + 100.05, 0.0, 0.0}});
    auto scene = assembler.value().assemble(snap_result.value(), in);
    REQUIRE(scene.ok());
    REQUIRE(scene.value().near_records.size() == 2);
    CHECK(scene.value().near_records[0].offset_m.v[0] == doctest::Approx(100.0f).epsilon(1e-6));
    // binary64 ulp at 1e13 m is ~1.9e-3 m, so the second offset is quantized
    // to that grid; centimetre agreement is the honest expectation.
    CHECK(std::abs(scene.value().near_records[1].offset_m.v[0] - 100.05) < 0.01);
}

TEST_CASE("near origins beyond the bound are rejected, not truncated") {
    auto assembler = SceneAssembler::create(1.0e9);
    REQUIRE(assembler.ok());
    auto snap_result = StaticFrameProvider{}.snapshot(makeInstant(0, 0.0));
    REQUIRE(snap_result.ok());

    auto in = basicInput(Vec3d{0.0, 0.0, 0.0});
    in.near_origins.push_back(BarycentricPosition{rootFrameId(), Vec3d{2.0e9, 0.0, 0.0}});
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

TEST_CASE("near geometry works through rotating frame snapshots") {
    RotatingFrameProvider provider;
    RotatingBodyParams params;
    params.id = FrameId{1};
    params.orbit_radius_m = 1.0e11;
    params.orbit_rate_rad_per_s = 1e-3;
    params.spin_rate_rad_per_s = 1e-2;
    CHECK(provider.addBody(params).ok());

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
    in.near_origins.push_back(
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

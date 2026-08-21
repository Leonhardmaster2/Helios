// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helios/math/mat4.hpp"
#include "helios/math/vec.hpp"

#include <cmath>

using namespace helios::math;

TEST_CASE("vector operations") {
    const Vec3f a{1, 2, 3};
    const Vec3f b{4, 5, 6};
    CHECK(dot(a, b) == 32.0f);
    CHECK(lengthSq(a) == 14.0f);
    CHECK(length(a) == doctest::Approx(std::sqrt(14.0f)));
    const Vec3f n = normalized(a);
    CHECK(length(n) == doctest::Approx(1.0f));
    const Vec3f c = cross(Vec3f{1, 0, 0}, Vec3f{0, 1, 0});
    CHECK(c.v[0] == doctest::Approx(0.0f));
    CHECK(c.v[1] == doctest::Approx(0.0f));
    CHECK(c.v[2] == doctest::Approx(1.0f));
}

TEST_CASE("matrix multiply and transform") {
    const Mat4f a = Mat4f::identity();
    const Vec4f v{1, 2, 3, 1};
    const Vec4f r = a * v;
    CHECK(r.v[0] == 1.0f);
    CHECK(r.v[3] == 1.0f);
    CHECK(transpose(a).m[0][0] == a.m[0][0]);

    const Mat4f rot = rotationZ(0.0f);
    CHECK(rot.m[0][0] == doctest::Approx(1.0f));
}

TEST_CASE("reversed-Z infinite perspective properties (D-002)") {
    // Projection: near maps to NDC z = 1, far (z -> -inf) maps to 0.
    const Mat4f proj = perspectiveReversedZInfinite(1.0f, 16.0f / 9.0f, 0.1f);
    auto project = [&](float z_view) {
        const Vec4f p = proj * Vec4f{0, 0, z_view, 1};
        return p.v[2] / p.v[3];
    };
    CHECK(project(-0.1f) == doctest::Approx(1.0f).epsilon(1e-5)); // near plane
    CHECK(project(-1.0f) == doctest::Approx(0.1f).epsilon(1e-5)); // 1 m
    CHECK(project(-1000.0f) == doctest::Approx(0.0001f).epsilon(1e-6));
    CHECK(project(-1e9f) > 0.0f); // asymptotic to 0, never negative
    // Monotonic: closer objects (more negative z) must have larger NDC z.
    CHECK(project(-1.0f) > project(-10.0f));
    CHECK(project(-10.0f) > project(-1000.0f));
    // w must be positive for objects in front.
    const Vec4f front = proj * Vec4f{0, 0, -5, 1};
    CHECK(front.v[3] > 0.0f);
}

TEST_CASE("lookAt view matrix") {
    const Mat4f view = lookAt(Vec3f{0, 0, 5}, Vec3f{0, 0, 0}, Vec3f{0, 1, 0});
    const Vec4f origin_in_view = view * Vec4f{0, 0, 0, 1};
    CHECK(origin_in_view.v[2] == doctest::Approx(-5.0f)); // origin is 5 units in front (-Z)
    const Vec4f eye_in_view = view * Vec4f{0, 0, 5, 1};
    CHECK(eye_in_view.v[2] == doctest::Approx(0.0f));
}

// ---------------------------------------------------------------------------
// M1: frame-tagged positions and quaternions (frames.hpp / quat.hpp)
// ---------------------------------------------------------------------------
#include "helios/math/frames.hpp"

#include <type_traits>

using helios::math::BodyFixedPosition;
using helios::math::FrameId;
using helios::math::Quatd;
using helios::math::Vec3d;

constexpr double kPi = 3.141592653589793;

TEST_CASE("same-frame position difference is checked and exact") {
    const FrameId earth{1};
    const BodyFixedPosition a{earth, Vec3d{1e7, 0.0, 0.0}};
    const BodyFixedPosition b{earth, Vec3d{1e7 + 3.5, 4.25, -1.0}};
    auto d = a.differenceTo(b);
    REQUIRE(d.ok());
    CHECK(d.value().v[0] == doctest::Approx(3.5));
    CHECK(d.value().v[1] == doctest::Approx(4.25));
    CHECK(d.value().v[2] == doctest::Approx(-1.0));
    // Round trip: displacedBy(differenceTo) returns the other position.
    const BodyFixedPosition b2 = a.displacedBy(d.value());
    auto back = b2.differenceTo(b);
    REQUIRE(back.ok());
    CHECK(length(back.value()) < 1e-6);
}

TEST_CASE("cross-frame position difference is rejected") {
    const BodyFixedPosition a{FrameId{1}, Vec3d{0, 0, 0}};
    const BodyFixedPosition b{FrameId{2}, Vec3d{1, 1, 1}};
    auto d = a.differenceTo(b);
    CHECK_FALSE(d.ok());
    CHECK(static_cast<int>(d.error().code()) == static_cast<int>(helios::core::ErrorCode::InvalidArgument));
}

TEST_CASE("cross-kind arithmetic is a type error (positions do not mix)") {
    // BarycentricPosition and BodyFixedPosition are distinct types with no
    // converting constructors or cross-type operators; this is enforced by
    // the type system itself, so there is nothing to exercise at runtime.
    static_assert(!std::is_convertible_v<helios::math::BarycentricPosition, BodyFixedPosition>);
}

TEST_CASE("quaternion basics: axis-angle rotates X to Y about Z") {
    const Quatd q = Quatd::fromAxisAngle(Vec3d{0, 0, 1}, kPi / 2.0);
    const Vec3d r = q.rotate(Vec3d{1, 0, 0});
    CHECK(r.v[0] == doctest::Approx(0.0).epsilon(1e-12));
    CHECK(r.v[1] == doctest::Approx(1.0).epsilon(1e-12));
    CHECK(r.v[2] == doctest::Approx(0.0).epsilon(1e-12));
}

TEST_CASE("quaternion composition applies rotations in order") {
    const Quatd qz = Quatd::fromAxisAngle(Vec3d{0, 0, 1}, 1.5707963267948966); // +90 deg
    const Quatd qx = Quatd::fromAxisAngle(Vec3d{1, 0, 0}, 1.5707963267948966); // +90 deg
    // (qz * qx) rotates about X first, then about Z.
    // (0,1,0) --about X--> (0,0,1) --about Z--> (0,0,1).
    const Vec3d r = (qz * qx).rotate(Vec3d{0, 1, 0});
    CHECK(r.v[0] == doctest::Approx(0.0).epsilon(1e-12));
    CHECK(r.v[1] == doctest::Approx(0.0).epsilon(1e-12));
    CHECK(r.v[2] == doctest::Approx(1.0).epsilon(1e-12));
}

TEST_CASE("quaternion inverse round-trips a rotation") {
    const Quatd q = Quatd::fromAxisAngle(Vec3d{0.3, -0.5, 0.8}, 0.7).normalized();
    const Vec3d v{2.0, -3.0, 1.5};
    const Vec3d r = q.conjugate().rotate(q.rotate(v));
    CHECK(r.v[0] == doctest::Approx(v.v[0]).epsilon(1e-12));
    CHECK(r.v[1] == doctest::Approx(v.v[1]).epsilon(1e-12));
    CHECK(r.v[2] == doctest::Approx(v.v[2]).epsilon(1e-12));
}

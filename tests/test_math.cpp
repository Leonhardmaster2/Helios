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
    CHECK(project(-0.1f) == doctest::Approx(1.0f).epsilon(1e-5));  // near plane
    CHECK(project(-1.0f) == doctest::Approx(0.1f).epsilon(1e-5));  // 1 m
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
    CHECK(origin_in_view.v[2] == doctest::Approx(5.0f)); // origin is 5 units in front
    const Vec4f eye_in_view = view * Vec4f{0, 0, 5, 1};
    CHECK(eye_in_view.v[2] == doctest::Approx(0.0f));
}

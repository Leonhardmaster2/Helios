// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/math/mat4.hpp"

#include <cmath>

namespace helios::math {

Mat4f perspectiveReversedZInfinite(float fov_y_radians, float aspect, float z_near) {
    const float f = 1.0f / std::tan(fov_y_radians * 0.5f);
    Mat4f r{};
    r.m[0][0] = f / aspect;
    r.m[1][1] = f;
    // Reversed-Z infinite far: z_clip = (near * z_view) / (z_view - far) with
    // far = inf gives: clip.w = -z_view, clip.z = near (constant) for z_view
    // in [-near, -inf): NDC z = near / -z_view -> 0 at infinity, 1 at near.
    r.m[2][2] = 0.0f;
    r.m[3][2] = z_near;
    r.m[2][3] = -1.0f; // w = -z_view
    return r;
}

Mat4f lookAt(Vec3f const& eye, Vec3f const& center, Vec3f const& up) {
    const Vec3f f = normalized(center - eye);
    const Vec3f s = normalized(cross(f, up));
    const Vec3f u = cross(s, f);
    Mat4f r = Mat4f::identity();
    r.m[0][0] = s.v[0];
    r.m[1][0] = s.v[1];
    r.m[2][0] = s.v[2];
    r.m[0][1] = u.v[0];
    r.m[1][1] = u.v[1];
    r.m[2][1] = u.v[2];
    r.m[0][2] = -f.v[0];
    r.m[1][2] = -f.v[1];
    r.m[2][2] = -f.v[2];
    r.m[3][0] = -dot(s, eye);
    r.m[3][1] = -dot(u, eye);
    r.m[3][2] = dot(f, eye);
    return r;
}

Mat4f rotationX(float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    Mat4f r = Mat4f::identity();
    r.m[1][1] = c;
    r.m[2][1] = -s;
    r.m[1][2] = s;
    r.m[2][2] = c;
    return r;
}

Mat4f rotationY(float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    Mat4f r = Mat4f::identity();
    r.m[0][0] = c;
    r.m[2][0] = s;
    r.m[0][2] = -s;
    r.m[2][2] = c;
    return r;
}

Mat4f rotationZ(float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    Mat4f r = Mat4f::identity();
    r.m[0][0] = c;
    r.m[1][0] = -s;
    r.m[0][1] = s;
    r.m[1][1] = c;
    return r;
}

} // namespace helios::math

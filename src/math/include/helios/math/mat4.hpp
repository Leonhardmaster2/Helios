// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#pragma once

#include "helios/math/vec.hpp"

namespace helios::math {

// Column-major 4x4 matrix (matches Vulkan/GLM conventions).
struct Mat4f {
    float m[4][4]{};

    static constexpr Mat4f identity() {
        Mat4f r{};
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
        return r;
    }

    constexpr float& operator()(std::size_t col, std::size_t row) { return m[col][row]; }
    constexpr float const& operator()(std::size_t col, std::size_t row) const { return m[col][row]; }
};

constexpr Mat4f operator*(Mat4f const& a, Mat4f const& b) {
    Mat4f r{};
    for (std::size_t c = 0; c < 4; ++c) {
        for (std::size_t row = 0; row < 4; ++row) {
            float s = 0.0f;
            for (std::size_t k = 0; k < 4; ++k) s += a.m[k][row] * b.m[c][k];
            r.m[c][row] = s;
        }
    }
    return r;
}

constexpr Vec4f operator*(Mat4f const& mat, Vec4f const& v) {
    Vec4f r{};
    for (std::size_t row = 0; row < 4; ++row) {
        float s = 0.0f;
        for (std::size_t c = 0; c < 4; ++c) s += mat.m[c][row] * v.v[c];
        r.v[row] = s;
    }
    return r;
}

constexpr Mat4f transpose(Mat4f const& a) {
    Mat4f r{};
    for (std::size_t c = 0; c < 4; ++c)
        for (std::size_t row = 0; row < 4; ++row) r.m[c][row] = a.m[row][c];
    return r;
}

// Right-handed perspective with reversed-Z (near maps to 1.0, far to 0.0)
// and infinite far plane — the accepted Helios depth direction (D-002).
Mat4f perspectiveReversedZInfinite(float fov_y_radians, float aspect, float z_near);

// Right-handed look-at (eye, center, up) in Vulkan clip conventions
// (forward = -Z; the returned matrix is a view matrix).
Mat4f lookAt(Vec3f const& eye, Vec3f const& center, Vec3f const& up);

// Simple rotation matrices (radians).
Mat4f rotationX(float radians);
Mat4f rotationY(float radians);
Mat4f rotationZ(float radians);

} // namespace helios::math

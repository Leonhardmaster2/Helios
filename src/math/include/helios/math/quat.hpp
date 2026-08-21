// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Double-precision quaternion for frame-tagged orientations. M1 uses this
// for camera poses and frame-snapshot rotations; float32 quaternions arrive
// only at the render boundary.
#pragma once

#include "helios/math/vec.hpp"

#include <cmath>

namespace helios::math {

struct Quatd {
    double w = 1.0;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    static constexpr Quatd identity() {
        return Quatd{};
    }

    static Quatd fromAxisAngle(Vec3d const& axis, double radians) {
        const double len = std::sqrt(axis.v[0] * axis.v[0] + axis.v[1] * axis.v[1] + axis.v[2] * axis.v[2]);
        if (len == 0.0) {
            return Quatd{};
        }
        const double half = 0.5 * radians;
        const double s = std::sin(half) / len;
        return Quatd{std::cos(half), s * axis.v[0], s * axis.v[1], s * axis.v[2]};
    }

    [[nodiscard]] constexpr Quatd conjugate() const {
        return Quatd{w, -x, -y, -z};
    }

    [[nodiscard]] constexpr double lengthSq() const {
        return w * w + x * x + y * y + z * z;
    }

    [[nodiscard]] double length() const {
        return std::sqrt(lengthSq());
    }

    [[nodiscard]] Quatd normalized() const {
        const double len = length();
        if (len == 0.0) {
            return Quatd{};
        }
        return Quatd{w / len, x / len, y / len, z / len};
    }

    // Hamilton product: (a * b) applies b first, then a.
    [[nodiscard]] constexpr Quatd operator*(Quatd const& o) const {
        return Quatd{w * o.w - x * o.x - y * o.y - z * o.z, w * o.x + x * o.w + y * o.z - z * o.y,
                     w * o.y - x * o.z + y * o.w + z * o.x, w * o.z + x * o.y - y * o.x + z * o.w};
    }

    // Rotate a vector by this (unit) quaternion.
    [[nodiscard]] constexpr Vec3d rotate(Vec3d const& v) const {
        const Vec3d u{x, y, z};
        // t = 2 * cross(u, v)
        const double tx = 2.0 * (u.v[1] * v.v[2] - u.v[2] * v.v[1]);
        const double ty = 2.0 * (u.v[2] * v.v[0] - u.v[0] * v.v[2]);
        const double tz = 2.0 * (u.v[0] * v.v[1] - u.v[1] * v.v[0]);
        return Vec3d{v.v[0] + w * tx + (u.v[1] * tz - u.v[2] * ty),
                     v.v[1] + w * ty + (u.v[2] * tx - u.v[0] * tz),
                     v.v[2] + w * tz + (u.v[0] * ty - u.v[1] * tx)};
    }
};

} // namespace helios::math

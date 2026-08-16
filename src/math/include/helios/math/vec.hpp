// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Minimal vector types for M0 smoke paths. Frame-tagged coordinate types
// (barycentric/body-fixed/tile-local) are Milestone 1 work and deliberately
// do not exist here.
#pragma once

#include <cmath>
#include <cstddef>

namespace helios::math {

template <typename T, std::size_t N> struct Vec {
    T v[N]{};

    constexpr T& operator[](std::size_t i) {
        return v[i];
    }
    constexpr T const& operator[](std::size_t i) const {
        return v[i];
    }

    constexpr Vec operator+(Vec const& o) const {
        Vec r{};
        for (std::size_t i = 0; i < N; ++i)
            r.v[i] = v[i] + o.v[i];
        return r;
    }
    constexpr Vec operator-(Vec const& o) const {
        Vec r{};
        for (std::size_t i = 0; i < N; ++i)
            r.v[i] = v[i] - o.v[i];
        return r;
    }
    constexpr Vec operator*(T s) const {
        Vec r{};
        for (std::size_t i = 0; i < N; ++i)
            r.v[i] = v[i] * s;
        return r;
    }
    constexpr Vec operator/(T s) const {
        Vec r{};
        for (std::size_t i = 0; i < N; ++i)
            r.v[i] = v[i] / s;
        return r;
    }
    constexpr Vec& operator+=(Vec const& o) {
        for (std::size_t i = 0; i < N; ++i)
            v[i] += o.v[i];
        return *this;
    }
    constexpr Vec& operator-=(Vec const& o) {
        for (std::size_t i = 0; i < N; ++i)
            v[i] -= o.v[i];
        return *this;
    }
};

template <typename T, std::size_t N> constexpr T dot(Vec<T, N> const& a, Vec<T, N> const& b) {
    T s{};
    for (std::size_t i = 0; i < N; ++i)
        s += a.v[i] * b.v[i];
    return s;
}

template <typename T, std::size_t N> constexpr T lengthSq(Vec<T, N> const& a) {
    return dot(a, a);
}

template <typename T, std::size_t N> T length(Vec<T, N> const& a) {
    return std::sqrt(lengthSq(a));
}

template <typename T, std::size_t N> Vec<T, N> normalized(Vec<T, N> const& a) {
    return a * (T{1} / length(a));
}

template <typename T> constexpr Vec<T, 3> cross(Vec<T, 3> const& a, Vec<T, 3> const& b) {
    return Vec<T, 3>{a.v[1] * b.v[2] - a.v[2] * b.v[1], a.v[2] * b.v[0] - a.v[0] * b.v[2],
                     a.v[0] * b.v[1] - a.v[1] * b.v[0]};
}

using Vec2f = Vec<float, 2>;
using Vec3f = Vec<float, 3>;
using Vec4f = Vec<float, 4>;
using Vec3d = Vec<double, 3>;
using Vec4d = Vec<double, 4>;

} // namespace helios::math

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Scene assembly boundary (docs/ARCHITECTURE.md "Precision contract"):
// transforms frame-tagged binary64 origins into camera-relative float32
// records with ONE controlled subtraction BEFORE any float conversion.
// Nothing here ever hands a huge absolute coordinate to float32, and no GPU
// shader ever subtracts astronomical positions.
#pragma once

#include "helios/core/error.hpp"
#include "helios/math/frames.hpp"
#include "helios/math/vec.hpp"
#include "helios/universe/frame_snapshot.hpp"

#include <vector>

namespace helios::render {

// Camera-relative near geometry record: bounded metres as binary32,
// suitable for direct use as a GPU vertex/instance offset.
struct NearRenderRecord {
    math::Vec3f offset_m{}; // camera -> object origin, float32, |offset| <= bound
};

// Distant bodies are never placed at a float32 astronomical coordinate:
// binary32 spacing near 1e12 m is ~65536 m. They are represented by their
// direction, angular radius, and analytic distance, all computed in double.
struct DistantBodyRecord {
    math::Vec3d direction_unit{}; // unit direction from camera to body centre (double)
    double distance_m = 0.0;      // centre distance along direction (double)
    double angular_radius_rad = 0.0;
};

struct DistantBodyInput {
    math::BarycentricPosition center{};
    double radius_m = 0.0;
};

struct SceneAssemblerInput {
    // Camera pose in its owning/storage frame plus the frame id of that
    // storage within the snapshot (rootFrameId() for barycentric storage).
    math::BarycentricCameraPose camera{};
    // Near geometry origins in any frames covered by the snapshot.
    std::vector<math::BarycentricPosition> near_origins;
    // Bodies too far for a meaningful float32 origin.
    std::vector<DistantBodyInput> distant_bodies;
    // Representation bound: origins whose camera-relative distance exceeds
    // this (metres) must NOT become float32 near records.
    double max_near_distance_m = 1.0e9;
};

struct AssembledScene {
    std::vector<NearRenderRecord> near_records;
    std::vector<DistantBodyRecord> distant_records;
};

class SceneAssembler {
  public:
    explicit SceneAssembler(double max_near_distance_m);

    [[nodiscard]] static core::Result<SceneAssembler> create(double max_near_distance_m);

    // Assembles one scene. Fails (InvalidArgument / OutOfRange) on:
    // non-finite inputs, zero/negative distant radius or non-positive
    // distance, and near origins beyond the representation bound (the caller
    // must reclassify those as distant or tile them).
    [[nodiscard]] core::Result<AssembledScene> assemble(universe::FrameSnapshot const& snapshot,
                                                        SceneAssemblerInput const& input) const;

  private:
    double max_near_distance_m_;
};

} // namespace helios::render

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Scene assembly boundary (docs/ARCHITECTURE.md "Precision contract"):
// transforms frame-tagged binary64 origins into camera-relative float32
// records with ONE controlled subtraction BEFORE any float conversion.
// Nothing here ever hands a huge absolute coordinate to float32, and no GPU
// shader ever subtracts astronomical positions.
//
// Frame support: the camera pose is given in an explicit STORAGE frame
// (barycentric root or any body-fixed frame present in the snapshot). Near
// origins and distant centres may be tagged barycentric OR body-fixed; the
// snapshot transforms them into the storage frame in double BEFORE the
// subtraction. Records are emitted in STORAGE-frame axes; the GPU view
// transform (rotation only) orients them.
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
// direction_unit is expressed in the STORAGE frame axes at assemble time.
struct DistantBodyRecord {
    math::Vec3d direction_unit{}; // unit direction from camera to body centre (double)
    double distance_m = 0.0;      // centre distance along direction (double)
    double angular_radius_rad = 0.0;
};

// Distant body centred in barycentric coordinates.
struct DistantBodyInput {
    math::BarycentricPosition center{};
    double radius_m = 0.0;
};

// Distant body centred in body-fixed coordinates of `center.frame()`.
struct DistantBodyInputFixed {
    math::BodyFixedPosition center{};
    double radius_m = 0.0;
};

struct SceneAssemblerInput {
    // Storage/owning frame of the camera: rootFrameId() for barycentric
    // storage, or a body-fixed frame id present in the snapshot. All
    // transformations and the final subtraction happen in this frame.
    math::FrameId storage_frame{};
    // Camera position in storage-frame coordinates (binary64).
    math::Vec3d camera_position{};
    // Camera orientation: rotates camera-local axes into storage axes.
    math::Quatd camera_orientation = math::Quatd::identity();

    // Near geometry origins, barycentric- and body-fixed-tagged. Both lists
    // are transformed through the snapshot into the storage frame first.
    std::vector<math::BarycentricPosition> near_barycentric;
    std::vector<math::BodyFixedPosition> near_body_fixed;

    // Bodies too far for a meaningful float32 origin.
    std::vector<DistantBodyInput> distant_bodies;
    std::vector<DistantBodyInputFixed> distant_body_fixed;

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

    // Assembles one scene. Fails (InvalidArgument / OutOfRange / NotFound)
    // on: non-finite inputs, unknown storage/body frames in the snapshot,
    // zero/negative distant radius or non-positive distance, and near
    // origins beyond the representation bound (the caller must reclassify
    // those as distant or tile them).
    [[nodiscard]] core::Result<AssembledScene> assemble(universe::FrameSnapshot const& snapshot,
                                                        SceneAssemblerInput const& input) const;

  private:
    double max_near_distance_m_;
};

} // namespace helios::render

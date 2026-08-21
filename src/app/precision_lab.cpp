// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/app/precision_lab.hpp"

#include "helios/assets/report.hpp"
#include "helios/core/log.hpp"
#include "helios/math/mat4.hpp"
#include "helios/render/device_probe.hpp"
#include "helios/render/reports.hpp"
#include "helios/render/scene_assembler.hpp"
#include "helios/universe/camera_reparent.hpp"
#include "helios/universe/frame_provider.hpp"
#include "helios/universe/instant.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace helios::app {
namespace {

using namespace helios::universe;
using helios::math::FrameId;
using helios::math::Quatd;
using helios::math::Vec3d;

constexpr double kPi = 3.141592653589793;
constexpr double kBodyRadius = 6.371e6; // synthetic body, Earth-scale
constexpr FrameId kBody{1};             // synthetic body-fixed frame id

// Thresholds (docs/VALIDATION.md P-003 proposal + M1 pixel budget).
struct Thresholds {
    double body_local_error_m = 1e-4;     // P-003: 0.1 mm same-body/local
    double root_roundtrip_error_m = 1e-2; // separate barycentric budget
    double pose_orientation_error_rad = 1e-9;
    double reparent_discontinuity_m = 1e-2; // root-frame pose jump per switch
    double reparent_drift_m = 1e-2;         // after 10k+ repeated transitions
    double jitter_stationary_px = 0.0;      // bitwise: stationary must be 0
    double jitter_cross_path_px = 0.5;      // storage-frame switch jitter
    double phase_increment_rad = 1e-9;      // 1 us increment at 1e7 days
};

double angleBetween(Quatd const& a, Quatd const& b) {
    Quatd rel = a.conjugate() * b;
    if (rel.w < 0.0) {
        rel = Quatd{-rel.w, -rel.x, -rel.y, -rel.z};
    }
    return 2.0 * std::acos(std::clamp(rel.w, -1.0, 1.0));
}

// Deterministic synthetic rotating body: 1 AU-scale orbit, Earth-day spin.
RotatingBodyParams makeSyntheticBody() {
    RotatingBodyParams b;
    b.id = kBody;
    b.orbit_radius_m = 1.0e11;
    b.orbit_rate_rad_per_s = 2 * kPi / 86400.0; // one revolution per day
    b.spin_rate_rad_per_s = 2 * kPi / 86400.0;
    return b;
}

FrameKinematics kinematicsFor(RotatingBodyParams const& b, Instant const& t) {
    FrameKinematics k;
    const double orbit_phase = RotatingFrameProvider::phaseFor(b.orbit_rate_rad_per_s, t);
    k.origin_velocity_m_per_s = Vec3d{-b.orbit_radius_m * b.orbit_rate_rad_per_s * std::sin(orbit_phase),
                                      b.orbit_radius_m * b.orbit_rate_rad_per_s * std::cos(orbit_phase), 0.0};
    k.angular_velocity_rad_per_s = Vec3d{0.0, 0.0, b.spin_rate_rad_per_s};
    return k;
}

// Double-precision pinhole projection (lab-local; the float32 pipeline is
// deliberately NOT used for jitter measurement).
struct DoubleProjection {
    double origin[3];
    double basis[3][3]; // right, up, forward (rows)
    double tan_half_fov_y;
    double width_px;
    double height_px;

    // Returns pixel coordinates; forward distance <= 0 yields nullopt-ish (-1).
    std::pair<double, double> project(Vec3d const& world) const {
        const double d[3] = {world.v[0] - origin[0], world.v[1] - origin[1], world.v[2] - origin[2]};
        const double cx = basis[0][0] * d[0] + basis[0][1] * d[1] + basis[0][2] * d[2];
        const double cy = basis[1][0] * d[0] + basis[1][1] * d[1] + basis[1][2] * d[2];
        const double cz = basis[2][0] * d[0] + basis[2][1] * d[1] + basis[2][2] * d[2];
        if (cz <= 0.0) {
            return {-1.0, -1.0};
        }
        const double ndc_x = (cx / (cz * tan_half_fov_y)); // x over tan*cz, aspect handled via height
        const double ndc_y = cy / (cz * tan_half_fov_y);
        return {width_px * (0.5 + 0.5 * ndc_x), height_px * (0.5 - 0.5 * ndc_y)};
    }
};

struct MeasurementAccumulator {
    double max_value = 0.0;
    double sum = 0.0;
    std::uint32_t count = 0;

    void add(double v) {
        max_value = std::max(max_value, v);
        sum += v;
        ++count;
    }
    [[nodiscard]] double average() const {
        return count > 0 ? sum / count : 0.0;
    }
};

} // namespace

int runPrecisionLab(CliOptions const& options) {
    core::logInfo("=== M1 precision laboratory (deterministic CPU route) ===");

    // ---- device probe (records the queried depth capability) ----
    auto probe = render::probeDeviceHeadless(options.validation);
    if (probe.failed()) {
        core::logError("device probe failed: {}", probe.error().to_string());
        return 2;
    }
    core::logInfo("device: {} (depth: {})", probe.value().gpu_name, probe.value().depth_format);

    const Thresholds thresholds;

    // ---- providers / reparenter ----
    RotatingFrameProvider provider;
    const RotatingBodyParams body = makeSyntheticBody();
    if (provider.addBody(body).failed()) {
        core::logError("provider setup failed");
        return 2;
    }
    ReparentThresholds reparent_thresholds;
    reparent_thresholds.enter_radius_m = 1.0e7; // enter body-fixed below this
    reparent_thresholds.exit_radius_m = 1.05e7; // exit above this (hysteresis)
    CameraReparenter reparenter(kBody, reparent_thresholds);

    // =====================================================================
    // 1. binary64 ULP spacing at the required ranges.
    // =====================================================================
    nlohmann::json ulp_table = nlohmann::json::array();
    for (double range : {1e7, 1e9, 1e12, 1e13}) {
        const double spacing = std::nextafter(range, std::numeric_limits<double>::infinity()) - range;
        ulp_table.push_back({{"range_m", range}, {"ulp_m", spacing}});
        core::logInfo("binary64 ULP at {:e} m: {:e} m", range, spacing);
    }

    // =====================================================================
    // 2. Instant/phase precision: 1 us increment at 1e7 days must stay
    //    observable in the rotating frame phase (VALIDATION.md astronomy
    //    suite: split representation, sub-second increments at large dates).
    // =====================================================================
    const std::int64_t huge_days = 10000000;
    auto t_base = Instant::make(huge_days, 40000.0);
    auto t_next = Instant::make(huge_days, 40000.0 + 1e-6);
    if (t_base.failed() || t_next.failed()) {
        core::logError("instant setup failed");
        return 2;
    }
    const double phase_a = RotatingFrameProvider::phaseFor(body.spin_rate_rad_per_s, t_base.value());
    const double phase_b = RotatingFrameProvider::phaseFor(body.spin_rate_rad_per_s, t_next.value());
    double phase_delta = phase_b - phase_a;
    if (phase_delta > kPi)
        phase_delta -= 2 * kPi;
    if (phase_delta < -kPi)
        phase_delta += 2 * kPi;
    const double expected_delta = body.spin_rate_rad_per_s * 1e-6;
    const bool phase_ok = std::abs(phase_delta - expected_delta) < thresholds.phase_increment_rad;
    core::logInfo("phase increment at 1e7 days: {:e} rad (expected {:e}) — {}", phase_delta, expected_delta,
                  phase_ok ? "PASS" : "FAIL");

    // =====================================================================
    // 3. Body-local transform error (P-003 0.1 mm budget): pure body-frame
    //    composition/inversion round trips, no root translation involved.
    // =====================================================================
    MeasurementAccumulator body_local;
    {
        FrameTransform local;
        local.rotation = Quatd::fromAxisAngle(Vec3d{0.3, -0.5, 0.8}, 0.7).normalized();
        local.translation = Vec3d{12.5, -3.25, 400.0}; // tile-local scale
        const Vec3d p{1.0, 2.0, 3.0};
        for (int i = 0; i < 1000; ++i) {
            const FrameTransform inv = invert(local);
            const Vec3d out = local.rotation.rotate(p) + local.translation;
            const Vec3d back = inv.rotation.rotate(out) + inv.translation;
            body_local.add(length(back - p));
        }
    }
    core::logInfo("body-local transform error: max {:e} m over {} trips", body_local.max_value,
                  body_local.count);

    // =====================================================================
    // 4. Root round-trip + pose round-trip error at each v1 range.
    // =====================================================================
    nlohmann::json range_results = nlohmann::json::array();
    const Instant route_epoch = Instant::make(0, 12345.678).value();
    auto epoch_snap = provider.snapshot(route_epoch);
    if (epoch_snap.failed()) {
        core::logError("snapshot failed: {}", epoch_snap.error().to_string());
        return 2;
    }
    auto body_to_root = epoch_snap.value().transformToRoot(kBody);
    if (body_to_root.failed()) {
        core::logError("body transform missing");
        return 2;
    }
    for (double altitude : {0.0, 1e7, 1e9, 1e12, 1e13}) {
        const double distance = kBodyRadius + altitude; // from body centre
        const Vec3d local_point{distance, 0.0, 0.0};
        const Vec3d root_point =
            body_to_root.value().rotation.rotate(local_point) + body_to_root.value().translation;

        // Position round trip root -> body -> root.
        auto back = epoch_snap.value().pointInFrame(
            kBody, rootFrameId(), epoch_snap.value().pointInFrame(rootFrameId(), kBody, root_point).value());
        const double pos_err = length(back.value() - root_point);

        // Camera orientation round trip.
        const Quatd cam_q = Quatd::fromAxisAngle(Vec3d{0.2, 0.4, -0.9}, 0.6).normalized();
        const Quatd q_body = (body_to_root.value().rotation.conjugate() * cam_q).normalized();
        const Quatd q_back = (body_to_root.value().rotation * q_body).normalized();
        const double ori_err = angleBetween(cam_q, q_back);

        range_results.push_back({{"altitude_m", altitude},
                                 {"distance_from_centre_m", distance},
                                 {"position_roundtrip_error_m", pos_err},
                                 {"orientation_roundtrip_error_rad", ori_err}});
        core::logInfo("range {:e} m: position roundtrip {:e} m, orientation {:e} rad", distance, pos_err,
                      ori_err);
    }

    // Root placement (quantization) error for a body centre at 1e13 m:
    // representable-value error for an irrational offset.
    double root_placement_err = 0.0;
    {
        const double centre = 1.0e13;
        for (double frac : {0.1, 0.25, 0.5, kPi, 0.987654321}) {
            const double represented = centre + frac;
            root_placement_err = std::max(root_placement_err, std::abs(represented - centre - frac));
        }
    }
    core::logInfo("root placement quantization error at 1e13 m: {:e} m", root_placement_err);

    // =====================================================================
    // 5. Camera reparenting route: surface -> orbit -> 1e9 -> 1e12 -> 1e13
    //    -> back to surface, crossing the hysteresis band repeatedly.
    // =====================================================================
    ReparentableCamera camera;
    camera.body_frame = kBody;
    camera.owner = OwningFrame::Barycentric;

    // Start on the surface (inside enter radius): first update enters.
    {
        auto snap = provider.snapshot(route_epoch);
        auto xf = snap.value().transformToRoot(kBody);
        const Vec3d surface_root =
            xf.value().rotation.rotate(Vec3d{kBodyRadius + 1.7, 0, 0}) + xf.value().translation;
        camera.barycentric.position = math::BarycentricPosition{rootFrameId(), surface_root};
        camera.barycentric.orientation = Quatd::fromAxisAngle(Vec3d{0, 1, 0}, 0.3).normalized();
    }

    // Waypoints as distances from body centre; the route crosses the band
    // [1e7, 1.05e7] twice on the way out and twice on the way back.
    const std::vector<double> route = {kBodyRadius + 1.7,
                                       9.9e6,
                                       1.02e7,
                                       9.95e6,
                                       1.08e7,
                                       1e9,
                                       1e12,
                                       1e13,
                                       1e9,
                                       1.02e7,
                                       9.9e6,
                                       1.02e7,
                                       9.95e6,
                                       1.08e7,
                                       kBodyRadius + 1.7};

    MeasurementAccumulator switch_jump;
    MeasurementAccumulator switch_angle;
    std::uint32_t transitions = 0;
    Vec3d route_dir{1.0, 0.0, 0.0};
    OwningFrame prev_owner = camera.owner;

    for (double dist : route) {
        auto snap = provider.snapshot(route_epoch);
        if (snap.failed()) {
            core::logError("route snapshot failed");
            return 2;
        }
        auto xf = snap.value().transformToRoot(kBody);
        // Teleport the authoritative pose to the waypoint distance.
        if (camera.owner == OwningFrame::Barycentric) {
            camera.barycentric.position =
                math::BarycentricPosition{rootFrameId(), xf.value().translation + route_dir * dist};
        } else {
            camera.body_fixed.position = math::BodyFixedPosition{kBody, route_dir * dist};
        }

        const Vec3d pos_before_root =
            camera.owner == OwningFrame::Barycentric
                ? camera.barycentric.position.coords()
                : xf.value().rotation.rotate(camera.body_fixed.position.coords()) + xf.value().translation;
        const Quatd ori_before_root =
            camera.owner == OwningFrame::Barycentric
                ? camera.barycentric.orientation
                : (xf.value().rotation * camera.body_fixed.orientation).normalized();

        const FrameKinematics kin = kinematicsFor(body, route_epoch);
        if (reparenter.update(snap.value(), kin, camera).failed()) {
            core::logError("reparenter update failed");
            return 2;
        }
        if (camera.owner != prev_owner) {
            ++transitions;
            const Vec3d pos_after_root =
                camera.owner == OwningFrame::Barycentric
                    ? camera.barycentric.position.coords()
                    : xf.value().rotation.rotate(camera.body_fixed.position.coords()) +
                          xf.value().translation;
            const Quatd ori_after_root =
                camera.owner == OwningFrame::Barycentric
                    ? camera.barycentric.orientation
                    : (xf.value().rotation * camera.body_fixed.orientation).normalized();
            switch_jump.add(length(pos_after_root - pos_before_root));
            switch_angle.add(angleBetween(ori_after_root, ori_before_root));
            prev_owner = camera.owner;
        }
    }
    core::logInfo("reparenting route: {} transitions, max pose jump {:e} m, max angle jump {:e} rad",
                  transitions, switch_jump.max_value, switch_angle.max_value);

    // Repeated transitions drift: 10001 band crossings, then compare the
    // final pose against a direct computation from the snapshot.
    double drift_pos = 0.0;
    double drift_ori = 0.0;
    {
        auto snap = provider.snapshot(route_epoch);
        auto xf = snap.value().transformToRoot(kBody);
        const FrameKinematics kin = kinematicsFor(body, route_epoch);
        // The loop ends on i=10000 (even) -> final target 0.995e7; the
        // drift reference must match that final commanded pose.
        const Vec3d reference_root = xf.value().translation + route_dir * 0.995e7;
        const Quatd reference_ori = Quatd::fromAxisAngle(Vec3d{0, 1, 0}, 0.3).normalized();
        for (int i = 0; i < 10001; ++i) {
            const double target = (i % 2 == 0) ? 0.995e7 : 1.08e7;
            if (camera.owner == OwningFrame::Barycentric) {
                camera.barycentric.position =
                    math::BarycentricPosition{rootFrameId(), xf.value().translation + route_dir * target};
            } else {
                camera.body_fixed.position = math::BodyFixedPosition{kBody, route_dir * target};
            }
            if (reparenter.update(snap.value(), kin, camera).failed()) {
                core::logError("repeated transition failed at {}", i);
                return 2;
            }
        }
        drift_pos = length(camera.barycentric.position.coords() - reference_root);
        drift_ori = angleBetween(camera.barycentric.orientation, reference_ori);
    }
    core::logInfo("drift after 10001 repeated transitions: {:e} m, {:e} rad", drift_pos, drift_ori);

    // =====================================================================
    // 6. Screen-space jitter: stationary camera, landmark fixed; compare the
    //    projection evaluated through barycentric storage against the same
    //    landmark evaluated through a body-frame storage round trip.
    // =====================================================================
    nlohmann::json jitter_results = nlohmann::json::array();
    double worst_cross_path_px = 0.0;
    double worst_stationary_px = 0.0;
    for (double altitude : {0.0, 1e7, 1e9, 1e12, 1e13}) {
        const double distance = kBodyRadius + altitude;
        auto snap = provider.snapshot(route_epoch);
        auto xf = snap.value().transformToRoot(kBody);

        // Camera 100 m offset from the landmark, looking along -X of the body.
        const Vec3d landmark_local{distance, 0.0, 0.0};
        const Vec3d landmark_root = xf.value().rotation.rotate(landmark_local) + xf.value().translation;
        const Vec3d cam_local{distance + 100.0, 0.0, 0.0};
        const Vec3d cam_root = xf.value().rotation.rotate(cam_local) + xf.value().translation;

        DoubleProjection proj;
        proj.origin[0] = cam_root.v[0];
        proj.origin[1] = cam_root.v[1];
        proj.origin[2] = cam_root.v[2];
        // Basis: forward = -X (toward landmark), up = +Z, right = forward x up.
        const Vec3d fwd{-1.0, 0.0, 0.0};
        const Vec3d up{0.0, 0.0, 1.0};
        const Vec3d right = cross(fwd, up);
        proj.basis[0][0] = right.v[0];
        proj.basis[0][1] = right.v[1];
        proj.basis[0][2] = right.v[2];
        proj.basis[1][0] = up.v[0];
        proj.basis[1][1] = up.v[1];
        proj.basis[1][2] = up.v[2];
        proj.basis[2][0] = fwd.v[0];
        proj.basis[2][1] = fwd.v[1];
        proj.basis[2][2] = fwd.v[2];
        proj.tan_half_fov_y = std::tan(0.5 * 60.0 * kPi / 180.0);
        proj.width_px = 1280.0;
        proj.height_px = 720.0;

        // Path A: direct root-frame evaluation (bitwise repeated 100x).
        const auto [ax, ay] = proj.project(landmark_root);
        double stationary_dev = 0.0;
        for (int i = 0; i < 100; ++i) {
            const auto [bx, by] = proj.project(landmark_root);
            stationary_dev = std::max(stationary_dev, std::abs(bx - ax) + std::abs(by - ay));
        }

        // Path B: evaluate through a body-frame round trip (what a camera
        // owned by the body frame effectively does every frame).
        auto lm_body = snap.value().pointInFrame(rootFrameId(), kBody, landmark_root);
        auto lm_back = snap.value().pointInFrame(kBody, rootFrameId(), lm_body.value());
        const auto [cx, cy] = proj.project(lm_back.value());
        const double cross_path_dev = std::abs(cx - ax) + std::abs(cy - ay);

        worst_stationary_px = std::max(worst_stationary_px, stationary_dev);
        worst_cross_path_px = std::max(worst_cross_path_px, cross_path_dev);
        jitter_results.push_back({{"altitude_m", altitude},
                                  {"stationary_deviation_px", stationary_dev},
                                  {"cross_path_deviation_px", cross_path_dev}});
        core::logInfo("jitter at altitude {:e} m: stationary {:e} px, cross-path {:e} px", altitude,
                      stationary_dev, cross_path_dev);
    }

    // =====================================================================
    // 7. Reversed-Z / infinite-far depth behaviour (analytic, D-002).
    // =====================================================================
    nlohmann::json depth_results = nlohmann::json::array();
    bool depth_monotonic = true;
    {
        const float near_plane = 0.1f;
        const helios::math::Mat4f depth_proj =
            helios::math::perspectiveReversedZInfinite(60.0f * float(kPi) / 180.0f, 16.0f / 9.0f, near_plane);
        auto project = [&](double view_z) {
            const helios::math::Vec4f p =
                depth_proj * helios::math::Vec4f{0.0f, 0.0f, static_cast<float>(-view_z), 1.0f};
            return static_cast<double>(p.v[2]) / static_cast<double>(p.v[3]);
        };
        double previous = 2.0;
        for (double d : {0.1, 1.0, 1e3, 1e6, 1e9, 1e12, 1e13}) {
            const double ndc = project(d);
            if (ndc > previous) {
                depth_monotonic = false; // reversed-Z: farther must be smaller
            }
            previous = ndc;
            depth_results.push_back({{"distance_m", d}, {"ndc_z", ndc}});
            core::logInfo("depth: d={:e} m -> ndc z={:e}", d, ndc);
        }
        const double asymptote = project(1e18);
        depth_results.push_back({{"distance_m", 1e18}, {"ndc_z", asymptote}});
        const bool asymptote_ok = asymptote >= 0.0 && asymptote < 1e-3;
        const bool near_ok = std::abs(project(near_plane) - 1.0) < 1e-5;

        // Near-plane sensitivity: record depth of a fixed 100 m point as the
        // near plane changes (informational; discontinuity is expected and
        // documented — the near plane is per-scene configuration).
        double near_change_delta = std::abs(project(100.0));
        depth_results.push_back({{"fixed_point_depth_at_near_0p1", near_change_delta}});

        if (!depth_monotonic || !asymptote_ok || !near_ok) {
            core::logError("depth analytic checks failed (mono={}, asymp={}, near={})", depth_monotonic,
                           asymptote_ok, near_ok);
        }
    }

    // =====================================================================
    // 8. Scene-assembly boundary sanity at range (subtraction before cast).
    // =====================================================================
    double assembler_local_err = 0.0;
    {
        auto assembler = render::SceneAssembler::create(1.0e9);
        if (assembler.failed()) {
            core::logError("scene assembler setup failed");
            return 2;
        }
        // Camera stored IN the body frame 100 m above the surface; landmark
        // body-fixed 150 m up: exercises the snapshot-backed storage-frame
        // subtraction path end to end.
        render::SceneAssemblerInput in;
        in.storage_frame = kBody;
        in.camera_position = Vec3d{kBodyRadius + 100.0, 0.0, 0.0};
        in.max_near_distance_m = 1.0e9;
        in.near_body_fixed.push_back(math::BodyFixedPosition{kBody, Vec3d{kBodyRadius + 150.0, 0.0, 0.0}});
        auto scene = assembler.value().assemble(epoch_snap.value(), in);
        if (scene.failed()) {
            core::logError("scene assembly failed: {}", scene.error().to_string());
            return 2;
        }
        // Same-frame difference: exactly the 50 m body-local offset.
        const math::Vec3d got{scene.value().near_records[0].offset_m.v[0],
                              scene.value().near_records[0].offset_m.v[1],
                              scene.value().near_records[0].offset_m.v[2]};
        assembler_local_err = length(got - Vec3d{50.0, 0.0, 0.0});
    }
    core::logInfo("scene-assembly near-offset error at surface scale: {:e} m", assembler_local_err);

    // =====================================================================
    // Pass/fail + evidence bundle.
    // =====================================================================
    const bool body_local_ok = body_local.max_value <= thresholds.body_local_error_m;
    const bool phase_check = phase_ok;
    const bool jump_ok = switch_jump.max_value <= thresholds.reparent_discontinuity_m;
    const bool angle_ok = switch_angle.max_value <= thresholds.pose_orientation_error_rad;
    const bool drift_ok =
        drift_pos <= thresholds.reparent_drift_m && drift_ori <= thresholds.pose_orientation_error_rad;
    const bool stationary_ok = worst_stationary_px <= thresholds.jitter_stationary_px;
    const bool cross_path_ok = worst_cross_path_px <= thresholds.jitter_cross_path_px;
    const bool assembler_ok = assembler_local_err <= thresholds.body_local_error_m;
    const bool overall = body_local_ok && phase_check && jump_ok && angle_ok && drift_ok && stationary_ok &&
                         cross_path_ok && assembler_ok && depth_monotonic;

    assets::EnvironmentInfo env = render::collectEnvironment("1280x720", probe.value().validation_enabled);
    env.gpu_name = probe.value().gpu_name;
    env.driver_version = probe.value().driver_info;

    nlohmann::json report;
    report["format"] = "helios-m1-precision-report";
    report["schema_version"] = 1;
    report["environment"] = assets::environmentToJson(env);
    report["device"] = nlohmann::json::parse(probe.value().device_report_json);
    report["validation"] = {{"errors", probe.value().validation_errors},
                            {"warnings", probe.value().validation_warnings},
                            {"layers_enabled", probe.value().validation_enabled}};
    report["scenario"] = {{"name", "m1-precision-lab"},
                          {"description",
                           "Deterministic CPU route: surface -> orbit -> 1e9 -> 1e12 -> 1e13 -> surface "
                           "with repeated hysteresis-band crossings; synthetic rotating body at 1e11 m"},
                          {"body",
                           {{"orbit_radius_m", body.orbit_radius_m},
                            {"spin_rate_rad_per_s", body.spin_rate_rad_per_s},
                            {"radius_m", kBodyRadius}}},
                          {"reparent_thresholds",
                           {{"enter_radius_m", reparent_thresholds.enter_radius_m},
                            {"exit_radius_m", reparent_thresholds.exit_radius_m}}},
                          {"route_waypoints_distance_from_centre_m", route},
                          {"repeated_transitions", 10001},
                          {"deterministic", true}};
    report["thresholds"] = {{"body_local_error_m", thresholds.body_local_error_m},
                            {"reparent_discontinuity_m", thresholds.reparent_discontinuity_m},
                            {"pose_orientation_error_rad", thresholds.pose_orientation_error_rad},
                            {"reparent_drift_m", thresholds.reparent_drift_m},
                            {"jitter_stationary_px", thresholds.jitter_stationary_px},
                            {"jitter_cross_path_px", thresholds.jitter_cross_path_px},
                            {"phase_increment_rad", thresholds.phase_increment_rad}};
    report["measurements"] = {{"binary64_ulp", ulp_table},
                              {"phase_increment_at_1e7_days_rad", phase_delta},
                              {"phase_increment_expected_rad", expected_delta},
                              {"body_local_transform_error_max_m", body_local.max_value},
                              {"root_roundtrips", range_results},
                              {"root_placement_quantization_error_m_at_1e13", root_placement_err},
                              {"reparenting",
                               {{"transitions", transitions},
                                {"max_pose_jump_m", switch_jump.max_value},
                                {"max_angle_jump_rad", switch_angle.max_value},
                                {"drift_after_10001_transitions_m", drift_pos},
                                {"drift_orientation_rad", drift_ori}}},
                              {"screen_space_jitter", jitter_results},
                              {"depth_analytics", depth_results},
                              {"depth_monotonic", depth_monotonic},
                              {"scene_assembly_near_offset_error_m", assembler_local_err}};
    report["pass_fail"] = {{"body_local_error", body_local_ok},
                           {"phase_increment_observable", phase_check},
                           {"reparent_discontinuity", jump_ok},
                           {"reparent_orientation", angle_ok},
                           {"reparent_drift", drift_ok},
                           {"jitter_stationary", stationary_ok},
                           {"jitter_cross_path", cross_path_ok},
                           {"depth_monotonic", depth_monotonic},
                           {"scene_assembly_local_error", assembler_ok},
                           {"overall", overall}};

    const std::string report_dir = options.report_dir.empty() ? "evidence/m1/local" : options.report_dir;
    std::error_code ec;
    fs::create_directories(report_dir, ec);
    auto w = assets::writeJsonReport((fs::path(report_dir) / "precision.json").string(), report);
    if (w.failed()) {
        core::logError("precision report write failed: {}", w.error().to_string());
        return 2;
    }
    // Device capabilities alongside (same schema as M0 bundles).
    nlohmann::json cap = nlohmann::json::object();
    cap["schema_version"] = 1;
    cap["environment"] = assets::environmentToJson(env);
    cap["device"] = nlohmann::json::parse(probe.value().device_report_json);
    (void)assets::writeJsonReport((fs::path(report_dir) / "capabilities.json").string(), cap);

    core::logInfo("M1 precision lab: {}", overall ? "ALL THRESHOLDS PASS" : "THRESHOLD FAILURES PRESENT");
    core::logInfo("validation: {} errors, {} warnings", probe.value().validation_errors,
                  probe.value().validation_warnings);
    core::logInfo("evidence written to {}", report_dir);
    return overall ? 0 : 3;
}

} // namespace helios::app

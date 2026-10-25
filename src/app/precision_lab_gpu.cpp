// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/app/precision_lab_gpu.hpp"

#include "helios/assets/report.hpp"
#include "helios/core/log.hpp"
#include "helios/math/frames.hpp"
#include "helios/render/gpu_precision.hpp"
#include "helios/render/gpu_precision_plan.hpp"
#include "helios/render/png_writer.hpp"
#include "helios/render/reports.hpp"
#include "helios/universe/camera_reparent.hpp"
#include "helios/universe/frame_provider.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace helios::app {
namespace {

using namespace helios::universe;
using namespace helios::render;
using helios::math::BarycentricPosition;
using helios::math::BodyFixedPosition;
using helios::math::FrameId;
using helios::math::Quatd;
using helios::math::Vec3d;

constexpr double kPi = 3.141592653589793;
constexpr double kBodyRadius = 6.371e6;
constexpr FrameId kBody{1};
constexpr float kFovYRad = 1.04719755f;  // 60 deg — must match the renderer
constexpr float kNearPlane = 0.1f;       // must match the renderer's proj
constexpr double kSwitchDistance = 2.5e8; // body mesh <-> impostor threshold
constexpr std::uint32_t kWidth = 800;
constexpr std::uint32_t kHeight = 450;

// Thresholds (fixed BEFORE the run; docs/VALIDATION.md M1 GPU section).
struct Thresholds {
    double landmark_err_px = 2.0;        // GPU blob centroid vs CPU-double projection
    double depth_rel_tol = 0.25;         // reversed-Z ndc tolerance (+ abs floor)
    double depth_abs_tol = 0.002;
    double micro_motion_tol_px = 0.25;   // measured vs analytic micro-motion shift
    std::uint32_t expected_transitions = 5; // deterministic route design
};

struct RouteWaypoint {
    double distance_from_centre_m;
    const char* name;
};

// Deterministic route: surface -> low orbit -> reparent band crossings ->
// out to 1e13 -> back to the surface, crossing the hysteresis band and the
// representation switch repeatedly.
const std::vector<RouteWaypoint> kRoute = {
    {kBodyRadius + 1.7, "surface"},        {7.0e6, "low"},
    {9.9e6, "pre-enter"},                  {1.02e7, "band-in"},
    {9.95e6, "band-out"},                  {1.08e7, "post-exit"},
    {2.5e8, "switch"},                     {1.0e9, "1e9"},
    {1.0e12, "1e12"},                      {1.0e13, "1e13"},
    {1.0e12, "back-1e12"},                 {1.0e9, "back-1e9"},
    {2.5e8, "back-switch"},                {1.02e7, "re-enter"},
    {9.95e6, "band-again"},                {1.08e7, "exit-again"},
    {7.0e6, "return-low"},                 {kBodyRadius + 1.7, "surface-return"},
};

RotatingBodyParams makeSyntheticBody() {
    RotatingBodyParams b;
    b.id = kBody;
    b.orbit_radius_m = 1.0e11;
    b.orbit_rate_rad_per_s = 2 * kPi / 86400.0;
    b.spin_rate_rad_per_s = 2 * kPi / 86400.0;
    return b;
}

FrameKinematics kinematicsFor(RotatingBodyParams const& b, Instant const& t) {
    FrameKinematics k;
    const double orbit_phase = RotatingFrameProvider::phaseFor(b.orbit_rate_rad_per_s, t);
    k.origin_velocity_m_per_s =
        Vec3d{-b.orbit_radius_m * b.orbit_rate_rad_per_s * std::sin(orbit_phase),
              b.orbit_radius_m * b.orbit_rate_rad_per_s * std::cos(orbit_phase), 0.0};
    k.angular_velocity_rad_per_s = Vec3d{0.0, 0.0, b.spin_rate_rad_per_s};
    return k;
}

// Quaternion from a storage-frame basis (images of local +x,+y,+z).
Quatd quatFromBasis(Vec3d x, Vec3d y, Vec3d z) {
    // Columns of the rotation matrix are x,y,z (verified against
    // Quatd::rotate by unit tests and the randomized round trip).
    const double m00 = x.v[0], m10 = x.v[1], m20 = x.v[2];
    const double m01 = y.v[0], m11 = y.v[1], m21 = y.v[2];
    const double m02 = z.v[0], m12 = z.v[1], m22 = z.v[2];
    const double tr = m00 + m11 + m22;
    Quatd q;
    if (tr > 0.0) {
        const double s = 0.5 / std::sqrt(tr + 1.0);
        q.w = 0.25 / s;
        q.x = (m21 - m12) * s;
        q.y = (m02 - m20) * s;
        q.z = (m10 - m01) * s;
    } else if (m00 > m11 && m00 > m22) {
        const double s = 2.0 * std::sqrt(1.0 + m00 - m11 - m22);
        q.w = (m21 - m12) / s;
        q.x = 0.25 * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const double s = 2.0 * std::sqrt(1.0 + m11 - m00 - m22);
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25 * s;
        q.z = (m12 + m21) / s;
    } else {
        const double s = 2.0 * std::sqrt(1.0 + m22 - m00 - m11);
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25 * s;
    }
    return q.normalized();
}

struct WaypointView {
    Vec3d cam_pos_storage{}; // authoritative pose in its OWN storage frame
    Vec3d cam_pos_root{};    // root-frame position (for scene math)
    OwningFrame owner{};
    Quatd orientation{};     // camera-local -> storage axes
};

bool computeWaypointCamera(FrameSnapshot const& snapshot, FrameKinematics const& kin,
                           CameraReparenter& reparenter, ReparentableCamera& camera,
                           std::uint32_t waypoint_index, double azimuth_deg, WaypointView& out) {
    auto xf = snapshot.transformToRoot(kBody);
    if (xf.failed())
        return false;
    const double az = azimuth_deg * kPi / 180.0;
    const Vec3d radial{std::cos(az), std::sin(az), 0.0};
    const double dist = kRoute[waypoint_index].distance_from_centre_m;

    if (camera.owner == OwningFrame::Barycentric) {
        camera.barycentric.position =
            BarycentricPosition{rootFrameId(), xf.value().translation + radial * dist};
    } else {
        camera.body_fixed.position = BodyFixedPosition{kBody, radial * dist};
    }
    if (reparenter.update(snapshot, kin, camera).failed())
        return false;

    out.owner = camera.owner;
    if (camera.owner == OwningFrame::Barycentric) {
        out.cam_pos_root = camera.barycentric.position.coords();
        out.cam_pos_storage = out.cam_pos_root;
    } else {
        out.cam_pos_storage = camera.body_fixed.position.coords();
        out.cam_pos_root = xf.value().rotation.rotate(out.cam_pos_storage) +
                           xf.value().translation;
    }

    // Camera looks at the body centre. Basis: right-handed with world
    // reference up = +Z (the body's spin axis); local forward is -z.
    Vec3d fwd_root = xf.value().translation - out.cam_pos_root;
    fwd_root = fwd_root * (1.0 / length(fwd_root));
    Vec3d z_root = fwd_root * -1.0;
    Vec3d right_root = cross(Vec3d{0.0, 0.0, 1.0}, z_root);
    right_root = right_root * (1.0 / length(right_root));
    Vec3d up_root = cross(z_root, right_root);
    up_root = up_root * (1.0 / length(up_root));

    if (camera.owner == OwningFrame::BodyFixed) {
        // Express the basis in BODY-frame axes (storage frame).
        const Quatd inv = xf.value().rotation.conjugate();
        out.orientation = quatFromBasis(inv.rotate(right_root), inv.rotate(up_root),
                                        inv.rotate(z_root));
    } else {
        out.orientation = quatFromBasis(right_root, up_root, z_root);
    }
    return true;
}

// Analytic occlusion: is the segment cam->point blocked by the sphere?
bool segmentOccludedBySphere(Vec3d const& cam, Vec3d const& point, Vec3d const& centre,
                             double radius) {
    const Vec3d d = point - cam;
    const double len = length(d);
    const Vec3d dir = d * (1.0 / len);
    const Vec3d m = cam - centre;
    const double b = dot(m, dir);
    const double c = dot(m, m) - radius * radius;
    if (c <= 0.0)
        return true; // camera inside the sphere: treat as occluded
    const double disc = b * b - c;
    if (disc <= 0.0)
        return false;
    const double t = -b - std::sqrt(disc);
    return t > 0.0 && t < len * 0.999;
}

struct CentroidResult {
    bool found = false;
    double x = 0.0;
    double y = 0.0;
    std::uint32_t pixels = 0;
};

// Blob search restricted to a window around the analytic position (avoids
// matching unrelated pixels elsewhere in the frame).
CentroidResult findColorCentroidNear(HostFrame const& frame, float const* color, double cx,
                                     double cy, double window_px) {
    const int r = static_cast<int>(color[0] * 255.0f + 0.5f);
    const int g = static_cast<int>(color[1] * 255.0f + 0.5f);
    const int b = static_cast<int>(color[2] * 255.0f + 0.5f);
    double sx = 0.0, sy = 0.0;
    std::uint32_t count = 0;
    const int x0 = std::max(0, static_cast<int>(cx - window_px));
    const int x1 = std::min(static_cast<int>(frame.width) - 1,
                            static_cast<int>(cx + window_px));
    const int y0 = std::max(0, static_cast<int>(cy - window_px));
    const int y1 = std::min(static_cast<int>(frame.height) - 1,
                            static_cast<int>(cy + window_px));
    for (int py = y0; py <= y1; ++py) {
        for (int px_i = x0; px_i <= x1; ++px_i) {
            const std::size_t o = (static_cast<std::size_t>(py) * frame.width + px_i) * 4;
            if (std::abs(frame.color[o] - r) <= 28 && std::abs(frame.color[o + 1] - g) <= 28 &&
                std::abs(frame.color[o + 2] - b) <= 28) {
                sx += px_i;
                sy += py;
                ++count;
            }
        }
    }
    CentroidResult c;
    if (count > 0) {
        c.found = true;
        c.x = sx / count;
        c.y = sy / count;
        c.pixels = count;
    }
    return c;
}

} // namespace

int runPrecisionLabGpu(CliOptions const& options) {
    core::logInfo("=== M1 GPU precision laboratory (real Vulkan render path) ===");
    const Thresholds thresholds;

    auto target = GPUPrecisionTarget::create(options.validation, kWidth, kHeight);
    if (target.failed()) {
        core::logError("gpu lab target creation failed");
        return 2;
    }

    RotatingFrameProvider provider;
    const RotatingBodyParams body = makeSyntheticBody();
    if (provider.addBody(body).failed()) {
        return 2;
    }
    const Instant epoch = Instant::make(0, 12345.678).value();
    auto epoch_snap = provider.snapshot(epoch);
    if (epoch_snap.failed()) {
        return 2;
    }
    const FrameKinematics kin = kinematicsFor(body, epoch);

    ReparentThresholds reparent_thresholds;
    reparent_thresholds.enter_radius_m = 1.0e7;
    reparent_thresholds.exit_radius_m = 1.05e7;
    CameraReparenter reparenter(kBody, reparent_thresholds);

    ReparentableCamera camera;
    camera.body_frame = kBody;
    camera.owner = OwningFrame::Barycentric;
    {
        auto xf = epoch_snap.value().transformToRoot(kBody);
        camera.barycentric.position =
            BarycentricPosition{rootFrameId(),
                                xf.value().translation + Vec3d{kBodyRadius + 1.7, 0, 0}};
    }

    // Distant star: fixed barycentric centre far away (always represented
    // through DistantBodyRecord, never a float32 position).
    DistantBodyInput star;
    star.center = BarycentricPosition{rootFrameId(), Vec3d{-1.0e12, 4.0e11, 1.5e10}};
    star.radius_m = 4.0e10;

    const float body_color[4] = {0.43f, 0.51f, 0.63f, 1.0f};
    const float impostor_body_color[4] = {1.0f, 0.59f, 0.16f, 1.0f}; // orange: distant rep
    const float star_color[4] = {0.90f, 0.90f, 0.94f, 1.0f};
    const float landmark_colors[3][4] = {{1.0f, 0.25f, 0.25f, 1.0f},
                                         {0.25f, 1.0f, 0.35f, 1.0f},
                                         {0.35f, 0.55f, 1.0f, 1.0f}};

    const PixelProjection base_proj{kWidth, kHeight, std::tan(kFovYRad * 0.5f), {}, {}};
    const auto assembler = SceneAssembler::create(kSwitchDistance * 1.05);

    const std::string report_dir =
        options.report_dir.empty() || options.report_dir == "evidence/m0/local"
            ? std::string("evidence/m1/gpu")
            : options.report_dir;
    const std::string captures_dir = (fs::path(report_dir) / "captures").string();
    std::error_code ec;
    fs::create_directories(captures_dir, ec);

    std::unique_ptr<FrameViewer> viewer;
    const bool windowed = options.windowed_smoke;
    if (windowed) {
        auto v = FrameViewer::create("Helios M1 GPU precision lab", kWidth, kHeight);
        if (v.failed()) {
            core::logError("viewer creation failed: {}", v.error().to_string());
            return 2;
        }
        viewer = std::move(v.value());
    }

    std::uint32_t transitions_total = 0;
    OwningFrame prev_owner = camera.owner;
    std::vector<nlohmann::json> waypoint_reports;
    std::vector<std::string> capture_paths;

    // Category accumulators.
    bool landmarks_all_ok = true;
    bool depth_all_ok = true;
    bool determinism_ok = false;
    bool reparenting_ok = true;
    bool micro_motion_ok = false;
    nlohmann::json micro_motion_detail = nlohmann::json::object();
    double worst_landmark_err_px = 0.0;

    // Spin phase at the epoch determines where landmarks must sit in the
    // BODY frame to be near the sub-camera point for a given azimuth.
    const double spin_phase =
        RotatingFrameProvider::phaseFor(body.spin_rate_rad_per_s, epoch);

    auto landmarkLocal = [&](std::uint32_t waypoint_index, std::size_t lm_idx) -> Vec3d {
        // Place landmarks around the sub-camera point: body-frame azimuth of
        // the camera = waypoint azimuth - spin phase. Offsets keep them well
        // inside the FOV and clear of the limb at every waypoint.
        static constexpr double kOffsetsDeg[3] = {0.0, 14.0, -11.0};
        static constexpr double kWarpLatDeg[3] = {0.0, 8.0, -6.0};
        const double az_body =
            waypoint_index * 25.0 * kPi / 180.0 - spin_phase +
            kOffsetsDeg[lm_idx] * kPi / 180.0;
        const double lat = kWarpLatDeg[lm_idx] * kPi / 180.0;
        return Vec3d{kBodyRadius * 1.002 * std::cos(lat) * std::cos(az_body),
                     kBodyRadius * 1.002 * std::sin(lat),
                     kBodyRadius * 1.002 * std::cos(lat) * std::sin(az_body)};
    };

    for (std::size_t wp_idx = 0; wp_idx < kRoute.size(); ++wp_idx) {
        auto snap = provider.snapshot(epoch);
        if (snap.failed())
            continue;
        WaypointView view;
        if (!computeWaypointCamera(snap.value(), kin, reparenter, camera,
                                   static_cast<std::uint32_t>(wp_idx), wp_idx * 25.0, view)) {
            core::logError("waypoint camera computation failed at {}", wp_idx);
            return 2;
        }
        if (view.owner != prev_owner) {
            ++transitions_total;
            prev_owner = view.owner;
        }
        const double dist = kRoute[wp_idx].distance_from_centre_m;
        const bool body_mesh = dist <= kSwitchDistance;
        const char* body_rep = body_mesh ? "near-mesh" : "impostor";
        const char* owner_name = view.owner == OwningFrame::Barycentric ? "BARY" : "BODY";
        const FrameId storage_frame =
            view.owner == OwningFrame::Barycentric ? rootFrameId() : kBody;

        // Assemble through SceneAssembler — the only source of rendered data.
        SceneAssemblerInput in;
        in.storage_frame = storage_frame;
        in.camera_position = view.cam_pos_storage;
        in.camera_orientation = view.orientation;
        in.max_near_distance_m = kSwitchDistance * 1.05;
        if (body_mesh) {
            in.near_body_fixed.push_back(BodyFixedPosition{kBody, Vec3d{0, 0, 0}});
            for (std::size_t i = 0; i < 3; ++i) {
                in.near_body_fixed.push_back(BodyFixedPosition{kBody, landmarkLocal(
                                                                          static_cast<std::uint32_t>(wp_idx), i)});
            }
        } else {
            auto centre = snap.value().transformToRoot(kBody);
            in.distant_bodies.push_back(DistantBodyInput{
                BarycentricPosition{rootFrameId(), centre.value().translation}, kBodyRadius});
        }
        in.distant_bodies.push_back(star);

        auto scene = assembler.value().assemble(snap.value(), in);
        if (scene.failed()) {
            core::logError("assembly failed at wp{}: {}", wp_idx, scene.error().to_string());
            return 2;
        }

        // Draw plan: meshes [body (if mesh)] + landmarks in assembly order;
        // impostors [body (if impostor)] then star.
        PlanOptions plan_options;
        plan_options.near_plane_m = kNearPlane;
        std::vector<float>& mc = plan_options.mesh_colors;
        std::vector<float>& mr = plan_options.mesh_radii;
        for (std::size_t i = 0; i < scene.value().near_records.size(); ++i) {
            if (body_mesh && i == 0) {
                mr.push_back(static_cast<float>(kBodyRadius));
                for (int c = 0; c < 4; ++c)
                    mc.push_back(body_color[c]);
            } else {
                const std::size_t lm_idx = i - (body_mesh ? 1 : 0);
                const Vec3d lm_storage = snap.value()
                                             .pointInFrame(kBody, storage_frame,
                                                           landmarkLocal(
                                                               static_cast<std::uint32_t>(wp_idx),
                                                               lm_idx))
                                             .value();
                const double d = length(lm_storage - in.camera_position);
                mr.push_back(static_cast<float>(d * 0.006));
                for (int c = 0; c < 4; ++c)
                    mc.push_back(landmark_colors[lm_idx][c]);
            }
        }
        for (std::size_t i = 0; i < scene.value().distant_records.size(); ++i) {
            const float* col = (!body_mesh && i == 0) ? impostor_body_color : star_color;
            for (int c = 0; c < 4; ++c)
                plan_options.impostor_colors.push_back(col[c]);
        }

        PixelProjection proj = base_proj;
        proj.camera_position = view.cam_pos_storage;
        proj.view = cameraViewFromOrientation(view.orientation);

        auto plan = buildFramePlan(scene.value(), view.orientation, proj, plan_options);
        if (plan.failed()) {
            core::logError("plan build failed: {}", plan.error().to_string());
            return 2;
        }

        HostFrame frame;
        if (target.value()->renderAndReadback(plan.value(), {0.02f, 0.02f, 0.04f, 1.0f}, frame)
                .failed()) {
            core::logError("render failed at wp{}", wp_idx);
            return 2;
        }

        nlohmann::json wp;
        wp["index"] = wp_idx;
        wp["name"] = kRoute[wp_idx].name;
        wp["distance_from_centre_m"] = dist;
        wp["altitude_m"] = dist - kBodyRadius;
        wp["owner_frame"] = owner_name;
        wp["storage_frame_id"] = storage_frame.value;
        wp["body_representation"] = body_rep;

        // ---- landmarks: visibility classification + centroid errors ----
        nlohmann::json landmarks_json = nlohmann::json::array();
        for (std::size_t i = 0; i < 3; ++i) {
            const Vec3d lm_local = landmarkLocal(static_cast<std::uint32_t>(wp_idx), i);
            nlohmann::json entry = {{"id", i}};
            if (!body_mesh) {
                // Beyond the representation bound landmarks are not
                // submitted at all: classified not_applicable this waypoint.
                entry["visibility"] = "not_applicable";
                landmarks_json.push_back(entry);
                continue;
            }
            const Vec3d lm_storage =
                snap.value().pointInFrame(kBody, storage_frame, lm_local).value();
            const auto [cpu_x, cpu_y] = projectToPixels(proj, lm_storage);
            entry["cpu_px_x"] = cpu_x;
            entry["cpu_px_y"] = cpu_y;
            const bool in_frame = cpu_x >= 40.0 && cpu_x < kWidth - 40.0 && cpu_y >= 30.0 &&
                                  cpu_y < kHeight - 30.0;
            const bool occluded =
                segmentOccludedBySphere(view.cam_pos_root,
                                        snap.value().pointInFrame(kBody, rootFrameId(), lm_local)
                                            .value(),
                                        snap.value().transformToRoot(kBody).value().translation,
                                        kBodyRadius);
            entry["occluded"] = occluded;
            if (!in_frame || occluded) {
                entry["visibility"] = "not_visible";
                landmarks_json.push_back(entry);
                continue;
            }
            entry["visibility"] = "visible";
            auto centroid = findColorCentroidNear(frame, landmark_colors[i], cpu_x, cpu_y, 24.0);
            if (!centroid.found) {
                // A required visible landmark missing from the render is a
                // failure — never silently treated as zero error.
                entry["found"] = false;
                landmarks_json.push_back(entry);
                landmarks_all_ok = false;
                continue;
            }
            const double err = std::hypot(centroid.x - cpu_x, centroid.y - cpu_y);
            worst_landmark_err_px = std::max(worst_landmark_err_px, err);
            entry["found"] = true;
            entry["gpu_px_x"] = centroid.x;
            entry["gpu_px_y"] = centroid.y;
            entry["blob_pixels"] = centroid.pixels;
            entry["err_px"] = err;
            if (err > thresholds.landmark_err_px) {
                landmarks_all_ok = false;
            }
            landmarks_json.push_back(entry);
        }
        wp["landmarks"] = landmarks_json;

        // ---- depth probe at screen centre (body centred every waypoint) --
        {
            const std::size_t cx = kWidth / 2, cy = kHeight / 2;
            const float depth_centre = frame.depth[cy * kWidth + cx];
            const double surface_dist = body_mesh ? dist - kBodyRadius : dist;
            const double expected = static_cast<double>(kNearPlane) / surface_dist;
            const double tol = std::max(thresholds.depth_abs_tol,
                                        expected * thresholds.depth_rel_tol);
            const bool ok = std::abs(static_cast<double>(depth_centre) - expected) <= tol &&
                            depth_centre > 0.0f;
            wp["depth_centre"] = depth_centre;
            wp["depth_centre_expected"] = expected;
            wp["depth_centre_ok"] = ok;
            if (!ok)
                depth_all_ok = false;

            // Star ordering: sample the star's centre pixel; the measured
            // depth must equal whichever of star/body is nearer along it.
            const Vec3d star_dir_storage =
                star.center.coords() - view.cam_pos_root;
            auto star_entry = scene.value().distant_records.back(); // star pushed last
            const auto [star_px_x, star_px_y] =
                projectToPixels(proj, star.center.coords());
            nlohmann::json order;
            if (star_px_x > 20.0 && star_px_x < kWidth - 20.0 && star_px_y > 20.0 &&
                star_px_y < kHeight - 20.0) {
                const float measured =
                    frame.depth[static_cast<std::size_t>(star_px_y) * kWidth +
                                static_cast<std::size_t>(star_px_x)];
                const double star_ndc = static_cast<double>(kNearPlane) /
                                        std::max(star_entry.distance_m, 1.0);
                // Is any body geometry covering that pixel? The body is
                // always at the frame centre; compute angular separation.
                const Vec3d body_dir_view = cameraViewFromOrientation(view.orientation).forward *
                                            -1.0; // towards body centre
                const Vec3d star_dir_view =
                    view.orientation.conjugate().rotate(
                        star_dir_storage * (1.0 / length(star_dir_storage)));
                const double sep = std::acos(std::clamp(
                    dot(star_dir_view, body_dir_view *
                                           (1.0 / length(body_dir_view))),
                    -1.0, 1.0));
                const double body_ang_rad =
                    std::asin(std::min(1.0, kBodyRadius / dist));
                const bool covered = sep <
                                     body_ang_rad + star_entry.angular_radius_rad;
                const double body_surf_dist = body_mesh ? dist - kBodyRadius : dist;
                const double expected_depth =
                    covered ? static_cast<double>(kNearPlane) / body_surf_dist : star_ndc;
                const double otol = std::max(thresholds.depth_abs_tol,
                                             expected_depth * thresholds.depth_rel_tol);
                const bool o_ok = std::abs(static_cast<double>(measured) - expected_depth) <=
                                      otol &&
                                  measured > 0.0f;
                order = {{"star_px_x", star_px_x},
                         {"star_px_y", star_px_y},
                         {"covered_by_body", covered},
                         {"expected_depth", expected_depth},
                         {"measured_depth", measured},
                         {"ordering_ok", o_ok}};
                if (!o_ok)
                    depth_all_ok = false;
            } else {
                order = {{"evaluated", false}, {"ordering_ok", true}};
            }
            wp["star_ordering"] = order;
        }

        // ---- stationary determinism at wp0 -------------------------------
        if (wp_idx == 0) {
            HostFrame frame2;
            if (target.value()
                    ->renderAndReadback(plan.value(), {0.02f, 0.02f, 0.04f, 1.0f}, frame2)
                    .failed()) {
                core::logError("stationary re-render failed");
                return 2;
            }
            std::uint32_t diff = 0;
            for (std::size_t b = 0; b < frame.color.size(); ++b) {
                if (frame.color[b] != frame2.color[b])
                    ++diff;
            }
            determinism_ok = diff == 0;
            wp["stationary_jitter"] = {{"differing_color_bytes", diff},
                                       {"byte_identical", determinism_ok}};
        }

        // ---- PNG captures for representative waypoints -------------------
        const bool is_capture = (wp_idx == 0 || wp_idx == 3 || wp_idx == 6 || wp_idx == 7 ||
                                 wp_idx == 9 || wp_idx == 17);
        if (is_capture && !windowed) {
            RgbaImage img;
            img.width = kWidth;
            img.height = kHeight;
            img.pixels = frame.color;
            drawOverlayText(img, 8, 6, "M1 GPU LAB HELIOS", 255, 255, 120);
            drawOverlayText(img, 8, 16,
                            std::format("WP{} {} D={:.3E}M", wp_idx, kRoute[wp_idx].name, dist),
                            220, 220, 220);
            drawOverlayText(img, 8, 26,
                            std::format("OWNER={} REP={} TRANS={}", owner_name, body_rep,
                                        transitions_total),
                            140, 220, 140);
            const std::string path = (fs::path(captures_dir) /
                                      std::format("wp{:02d}_{}.png", wp_idx,
                                                  kRoute[wp_idx].name))
                                         .string();
            if (writePngRgba(path, img).ok()) {
                capture_paths.push_back(path);
                wp["capture"] = path;
            }
        }

        // Windowed viewing (interactive step-through of the same route).
        if (windowed && viewer) {
            if (!viewer->show(frame)) {
                break;
            }
            SDL_Delay(900);
        }

        wp["validation_errors"] = target.value()->validationErrors();
        wp["validation_warnings"] = target.value()->validationWarnings();
        waypoint_reports.push_back(wp);
    }

    // ---- micro-motion: controlled yaw at the first mesh waypoint --------
    {
        auto snap = provider.snapshot(epoch);
        WaypointView view;
        if (computeWaypointCamera(snap.value(), kin, reparenter, camera, 0, 0.0, view)) {
            const FrameId sf = view.owner == OwningFrame::Barycentric ? rootFrameId() : kBody;
            const double delta_yaw = 1.0e-3; // rad, about the camera's local Y
            Quatd orientation2 =
                (view.orientation * Quatd::fromAxisAngle(Vec3d{0.0, 1.0, 0.0}, delta_yaw))
                    .normalized();

            auto assemble_and_project = [&](Quatd const& orientation) {
                SceneAssemblerInput in;
                in.storage_frame = sf;
                in.camera_position = view.cam_pos_storage;
                in.camera_orientation = orientation;
                in.max_near_distance_m = kSwitchDistance * 1.05;
                in.near_body_fixed.push_back(BodyFixedPosition{kBody, Vec3d{0, 0, 0}});
                in.near_body_fixed.push_back(BodyFixedPosition{kBody, landmarkLocal(0, 0)});
                auto sc = assembler.value().assemble(snap.value(), in);
                if (sc.failed())
                    return std::pair<double, double>{-2.0, -2.0};
                PixelProjection pr = base_proj;
                pr.camera_position = view.cam_pos_storage;
                pr.view = cameraViewFromOrientation(orientation);
                PlanOptions po;
                po.near_plane_m = kNearPlane;
                po.mesh_colors = {body_color[0], body_color[1], body_color[2], body_color[3],
                                  landmark_colors[0][0], landmark_colors[0][1],
                                  landmark_colors[0][2], landmark_colors[0][3]};
                const Vec3d lm_storage =
                    snap.value().pointInFrame(kBody, sf, landmarkLocal(0, 0)).value();
                const double d = length(lm_storage - view.cam_pos_storage);
                po.mesh_radii = {static_cast<float>(kBodyRadius),
                                 static_cast<float>(d * 0.006)};
                auto pl = buildFramePlan(sc.value(), orientation, pr, po);
                if (pl.failed())
                    return std::pair<double, double>{-3.0, -3.0};
                HostFrame fr;
                if (target.value()
                        ->renderAndReadback(pl.value(), {0.02f, 0.02f, 0.04f, 1.0f}, fr)
                        .failed())
                    return std::pair<double, double>{-4.0, -4.0};
                PixelProjection pp = pr;
                auto c = findColorCentroidNear(fr, landmark_colors[0],
                                               projectToPixels(pp, lm_storage).first,
                                               projectToPixels(pp, lm_storage).second, 24.0);
                if (!c.found)
                    return std::pair<double, double>{-5.0, -5.0};
                return std::pair<double, double>{c.x, c.y};
            };

            const auto [x0, y0] = assemble_and_project(view.orientation);
            const auto [x1, y1] = assemble_and_project(orientation2);
            PixelProjection pr = base_proj;
            pr.camera_position = view.cam_pos_storage;
            pr.view = cameraViewFromOrientation(view.orientation);
            const Vec3d lm_storage = snap.value().pointInFrame(kBody, sf, landmarkLocal(0, 0)).value();
            const auto [ax0, ay0] = projectToPixels(pr, lm_storage);
            PixelProjection pr2 = base_proj;
            pr2.camera_position = view.cam_pos_storage;
            pr2.view = cameraViewFromOrientation(orientation2);
            const auto [ax1, ay1] = projectToPixels(pr2, lm_storage);
            const double analytic = std::hypot(ax1 - ax0, ay1 - ay0);
            const double measured = std::hypot(x1 - x0, y1 - y0);
            micro_motion_ok = x0 >= 0.0 && x1 >= 0.0 &&
                              std::abs(measured - analytic) <= thresholds.micro_motion_tol_px;
            micro_motion_detail = {{"delta_yaw_rad", delta_yaw},
                                 {"analytic_shift_px", analytic},
                                 {"measured_shift_px", measured},
                                 {"tolerance_px", thresholds.micro_motion_tol_px},
                                 {"ok", micro_motion_ok}};
        }
    }

    // ---- evidence gate ----------------------------------------------------
    const bool validation_ok = target.value()->validationErrors() == 0 &&
                               target.value()->validationWarnings() == 0;
    reparenting_ok = transitions_total == thresholds.expected_transitions;
    const bool overall = validation_ok && landmarks_all_ok && depth_all_ok &&
                         determinism_ok && reparenting_ok && micro_motion_ok;

    assets::EnvironmentInfo env = collectEnvironment("800x450", options.validation);
    env.gpu_name = target.value()->deviceName();

    nlohmann::json route_json = nlohmann::json::array();
    for (auto const& rwp : kRoute) {
        route_json.push_back(
            {{"distance_from_centre_m", rwp.distance_from_centre_m}, {"name", rwp.name}});
    }

    nlohmann::json report;
    report["format"] = "helios-m1-gpu-report";
    report["schema_version"] = 1;
    report["environment"] = assets::environmentToJson(env);
    report["git_hash"] = env.helios_git_hash;
    report["device"] = target.value()->deviceName();
    report["resolution"] = {{"width", kWidth}, {"height", kHeight}};
    report["route"] = route_json;
    report["thresholds"] = {
        {"landmark_err_px", thresholds.landmark_err_px},
        {"depth_rel_tol", thresholds.depth_rel_tol},
        {"depth_abs_tol", thresholds.depth_abs_tol},
        {"micro_motion_tol_px", thresholds.micro_motion_tol_px},
        {"expected_transitions", thresholds.expected_transitions}};
    report["waypoints"] = waypoint_reports;
    report["captures"] = capture_paths;
    report["micro_motion"] = micro_motion_detail;
    report["validation"] = {{"errors", target.value()->validationErrors()},
                            {"warnings", target.value()->validationWarnings()}};
    report["transitions_total"] = transitions_total;
    report["pass_fail"] = {
        {"validation_clean", validation_ok},
        {"landmarks", landmarks_all_ok},
        {"depth_checks", depth_all_ok},
        {"determinism", determinism_ok},
        {"reparenting", reparenting_ok},
        {"micro_motion", micro_motion_ok},
        {"overall", overall}};

    auto write_ok =
        assets::writeJsonReport((fs::path(report_dir) / "precision_gpu.json").string(), report);
    if (write_ok.failed()) {
        core::logError("gpu report write failed: {}", write_ok.error().to_string());
        return 2;
    }

    core::logInfo(
        "GPU lab done: {} waypoints, {} transitions, {} captures; gate: landmarks={} "
        "depth={} determinism={} reparenting={} micro_motion={} validation={}",
        waypoint_reports.size(), transitions_total, capture_paths.size(), landmarks_all_ok,
        depth_all_ok, determinism_ok, reparenting_ok, micro_motion_ok, validation_ok);
    core::logInfo("worst landmark error: {:.3f} px; evidence written to {}",
                  worst_landmark_err_px, report_dir);
    return overall ? 0 : 3;
}

} // namespace helios::app

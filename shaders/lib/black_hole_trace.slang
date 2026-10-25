// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Real-time Schwarzschild black-hole lab: geodesic integration and shading
// shared by the trace pass (black_hole.slang) and the adaptive refinement pass
// (black_hole_refine.slang).
//
// Geometry.  Spherical symmetry confines every ray to the plane spanned by the
// camera's radial direction and the ray.  In that plane, with u = M/r and the
// orbital angle phi as the independent variable, a null geodesic obeys the
// exact Binet equation
//
//     d2u/dphi2 = -u + 3 u^2 .
//
// It is integrated with classical RK4 in float32.  The thin disk lies in the
// z = 0 plane, which the orbital plane meets along a line through the origin,
// so every disk-plane crossing happens at phi = phi_0 + k*pi.  Steps are made
// to land exactly on those angles; a crossing is therefore an evaluation of
// u, not a search, and the k-th crossing is the k-th order image of the disk.
// Rays end at u <= 0 (infinity, giving the asymptotic sky direction) or once
// they are ingoing inside the photon sphere (capture is then inevitable).
//
// Radiometry.  The disk is a multi-temperature black body with the
// Page-Thorne zero-torque profile.  The frequency ratio g uses the conserved
// photon energy and axial angular momentum, so Doppler boosting, gravitational
// redshift and time dilation are exact for circular-geodesic gas and a static
// camera; a black body at T is observed as a black body at g*T.  Colours come
// from a Planck-spectrum table built on the CPU (helios/blackhole/blackbody).
//
// Not modelled: spin, disk thickness, self-irradiation, radiative transfer,
// light travel-time delay, or a measured star catalogue.  The turbulence
// pattern and the star field are procedural presentation content.
#pragma once

#include "sky.slang"

struct BlackHoleFrame {
    // xyz = static-observer position (M); w = disk animation time (M)
    float4 camera_position;
    // xyz = camera right * tan(fov_y / 2) * aspect
    float4 camera_right;
    // xyz = camera up * tan(fov_y / 2)
    float4 camera_up;
    // xyz = camera forward (unit)
    float4 camera_forward;
    // x = width (px), y = height (px), z = 1 / width, w = 1 / height
    float4 viewport;
    // x = inner radius (M), y = outer radius (M), z = peak temperature (K),
    // w = opacity in [0, 1]; 0 disables the disk
    float4 disk;
    // x = turbulence amplitude, y = limb darkening (0/1),
    // z = 1 / peak Novikov-Thorne flux, w = outer fade start as a fraction of y
    float4 disk_detail;
    // x = star intensity, y = galactic band intensity,
    // z = mode (0 stars, 1 celestial grid, 2 black), w = unused
    float4 sky;
    // x = ln(reference luminance), y = ln(table minimum K),
    // z = 1 / (ln(table maximum K) - y), w = table texel count
    float4 radiometry;
    // x = maximum RK4 step (rad), y = maximum disk-plane crossings,
    // z = samples per pixel, w = extra samples near the critical curve
    float4 integration;
    // x = Doppler term (0/1), y = gravitational + time-dilation term (0/1),
    // z = space-time curvature (1 Schwarzschild, 0 flat comparison),
    // w = output (0 radiance, 1 geometry record, 2 step-count heat map)
    float4 effects;
    // x = display exposure used by the refine pass's contrast test,
    // y = contrast threshold in display units, z = 1 when the refine pass
    // runs (this pass then takes one sample per pixel), w = unused
    float4 refine;
};

[[vk::binding(0, 0)]] ConstantBuffer<BlackHoleFrame> frame;
[[vk::binding(1, 0)]] Texture2D<float4> blackbody_table;

static const float CRITICAL_IMPACT = 5.19615242f; // 3 sqrt(3) M
static const int MAX_HITS = 3;
static const int MAX_STEPS = 512;

static const int TERMINATION_CAPTURED = 0;
static const int TERMINATION_ESCAPED = 1;
static const int TERMINATION_BLOCKED = 2;
struct VsOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 blackbodyEntry(float kelvin) {
    return blackbodyEntry(blackbody_table, frame.radiometry, kelvin);
}

float3 blackbodyRadiance(float kelvin) {
    return blackbodyRadiance(blackbody_table, frame.radiometry, kelvin);
}

float3 skyRadiance(float3 direction, SkyFootprint footprint, float lapse) {
    return skyRadiance(blackbody_table, frame.radiometry, frame.sky, direction, footprint, lapse);
}

// ---------------------------------------------------------------------------
// Geodesic integration
// ---------------------------------------------------------------------------

struct Trace {
    float3 sky_dir;             // asymptotic direction when escaped
    int termination;
    int hit_count;
    float hit_radius[MAX_HITS];
    float2 hit_xy[MAX_HITS];    // disk-plane position of each crossing (M)
    float impact;               // b = L / E
    float axial;                // lambda = L_z / E of the photon reaching the camera
    float tilt;                 // sqrt(1 - (L_z / L)^2)
    float lapse;                // sqrt(1 - 2M/r) of the static camera
    float steps;
};

// The rim is not a uniform fade: `structure` (the turbulence field, in
// [-1, 1]) moves the outer boundary by up to RIM_REACH of the radius, so the
// disk ends in streamers.  Crossings are therefore recorded out to
// DISK_RECORD_LIMIT times the nominal outer radius.
static const float RIM_REACH = 0.16f;
static const float DISK_RECORD_LIMIT = 1.2f;

float diskAlpha(float radius, float structure) {
    float inner = smoothstep(frame.disk.x, frame.disk.x + 0.35f, radius);
    float effective = radius * (1.0f + RIM_REACH * frame.disk_detail.x * structure);
    float outer = 1.0f - smoothstep(frame.disk.y * frame.disk_detail.w, frame.disk.y, effective);
    return frame.disk.w * inner * outer;
}

// True only where no rim structure can make the disk less than opaque, which
// lets the integrator stop without evaluating the turbulence field.
bool diskCertainlyOpaque(float radius) {
    return frame.disk.w > 0.995f && radius > frame.disk.x + 0.35f &&
           radius * (1.0f + RIM_REACH) < frame.disk.y * frame.disk_detail.w;
}

Trace traceRay(float3 direction) {
    Trace trace;
    trace.sky_dir = direction;
    trace.termination = TERMINATION_CAPTURED;
    trace.hit_count = 0;
    for (int slot = 0; slot < MAX_HITS; ++slot) {
        trace.hit_radius[slot] = 0.0f;
        trace.hit_xy[slot] = float2(0.0f, 0.0f);
    }
    trace.impact = 0.0f;
    trace.axial = 0.0f;
    trace.tilt = 0.0f;
    trace.steps = 0.0f;

    float curvature = frame.effects.z;
    float3 camera = frame.camera_position.xyz;
    float camera_radius = length(camera);
    float3 radial = camera / camera_radius;
    float u0 = 1.0f / camera_radius;
    float lapse = sqrt(max(1.0f - 2.0f * u0 * curvature, 1.0e-6f));
    trace.lapse = lapse;

    // Decompose the local static-observer direction into radial and tangential parts.
    float cos_alpha = dot(direction, radial);
    float3 tangential = direction - cos_alpha * radial;
    float sin_alpha = length(tangential);
    if (sin_alpha < 1.0e-5f) {
        // Purely radial rays never leave the camera's radial line.
        trace.termination = cos_alpha > 0.0f ? TERMINATION_ESCAPED : TERMINATION_CAPTURED;
        return trace;
    }
    float3 tangent = tangential / sin_alpha;
    float3 plane_normal = cross(radial, tangent);
    float impact = sin_alpha / (u0 * lapse);
    trace.impact = impact;
    // The traced ray runs backwards in time, so the photon that actually reaches
    // the camera orbits the other way: L = -b E * plane_normal.
    trace.axial = -impact * plane_normal.z;
    trace.tilt = sqrt(saturate(1.0f - plane_normal.z * plane_normal.z));

    float u = u0;
    float v = -u0 * lapse * cos_alpha / sin_alpha; // du/dphi
    float phi = 0.0f;

    // z(phi) is proportional to radial.z cos(phi) + tangent.z sin(phi).
    float next_crossing = 1.0e9f;
    float plane_amplitude = radial.z * radial.z + tangent.z * tangent.z;
    if (frame.disk.w > 0.0f && plane_amplitude > 1.0e-10f) {
        next_crossing = atan2(-radial.z, tangent.z);
        next_crossing -= PI * floor(next_crossing / PI);
        if (next_crossing < 1.0e-4f) {
            next_crossing += PI;
        }
    }

    float max_step = frame.integration.x;
    int max_crossings = int(frame.integration.y);
    int crossings = 0;
    float curve = 3.0f * curvature;
    // Flat comparison: an opaque sphere of the horizon's coordinate radius.
    float capture_u = curvature > 0.5f ? (1.0f / 3.0f) : 0.5f;

    // A ray still orbiting a full turn after its last possible disk crossing
    // lies in an exponentially thin band around the critical curve; it is
    // treated as captured instead of being followed for hundreds of steps.
    float angle_limit = (float(max_crossings) + 2.0f) * PI;

    for (int step = 0; step < MAX_STEPS; ++step) {
        if (phi > angle_limit) {
            break;
        }
        float remaining = next_crossing - phi;
        bool landing = remaining <= max_step * 1.001f;
        float h = landing ? remaining : max_step;

        float k1u = v;
        float k1v = u * (curve * u - 1.0f);
        float ua = u + 0.5f * h * k1u;
        float k2u = v + 0.5f * h * k1v;
        float k2v = ua * (curve * ua - 1.0f);
        float ub = u + 0.5f * h * k2u;
        float k3u = v + 0.5f * h * k2v;
        float k3v = ub * (curve * ub - 1.0f);
        float uc = u + h * k3u;
        float k4u = v + h * k3v;
        float k4v = uc * (curve * uc - 1.0f);
        float next_u = u + h * (k1u + 2.0f * (k2u + k3u) + k4u) * (1.0f / 6.0f);
        float next_v = v + h * (k1v + 2.0f * (k2v + k3v) + k4v) * (1.0f / 6.0f);
        trace.steps += 1.0f;

        if (next_u <= 0.0f) {
            // Escaped to infinity inside this step: locate u = 0 on the cubic
            // Hermite interpolant, starting from the secant estimate.
            float s = u / (u - next_u);
            float hv0 = h * v;
            float hv1 = h * next_v;
            for (int newton = 0; newton < 2; ++newton) {
                float s2 = s * s;
                float s3 = s2 * s;
                float value = (2.0f * s3 - 3.0f * s2 + 1.0f) * u + (s3 - 2.0f * s2 + s) * hv0 +
                              (3.0f * s2 - 2.0f * s3) * next_u + (s3 - s2) * hv1;
                float slope = (6.0f * s2 - 6.0f * s) * (u - next_u) +
                              (3.0f * s2 - 4.0f * s + 1.0f) * hv0 + (3.0f * s2 - 2.0f * s) * hv1;
                s = saturate(s - value / min(slope, -1.0e-12f));
            }
            float escape_angle = phi + s * h;
            trace.sky_dir = cos(escape_angle) * radial + sin(escape_angle) * tangent;
            trace.termination = TERMINATION_ESCAPED;
            break;
        }
        if (next_u > capture_u && (next_v > 0.0f || curvature < 0.5f)) {
            break; // captured
        }

        u = next_u;
        v = next_v;
        phi += h;

        if (landing) {
            phi = next_crossing;
            float radius = 1.0f / u;
            if (radius > frame.disk.x && radius < frame.disk.y * DISK_RECORD_LIMIT) {
                if (trace.hit_count < MAX_HITS) {
                    float3 position = radius * (cos(phi) * radial + sin(phi) * tangent);
                    trace.hit_radius[trace.hit_count] = radius;
                    trace.hit_xy[trace.hit_count] = position.xy;
                    trace.hit_count += 1;
                }
                if (diskCertainlyOpaque(radius)) {
                    trace.termination = TERMINATION_BLOCKED;
                    break;
                }
            }
            crossings += 1;
            // Past the configured image order the ray is still followed to its
            // end so the sky classification stays correct.
            next_crossing = crossings >= max_crossings ? 1.0e9f : next_crossing + PI;
        }
    }
    return trace;
}

// ---------------------------------------------------------------------------
// Accretion disk
// ---------------------------------------------------------------------------

// Page & Thorne (1974), a* = 0, in units of Mdot / M^2.  Mirrors
// helios::blackhole::novikovThorneFlux.
float novikovThorneFlux(float radius) {
    if (radius <= 6.0f) {
        return 0.0f;
    }
    float x = sqrt(radius);
    float delta = x - 2.4494897f;
    float bracket;
    if (delta < 0.03f) {
        // Series about the ISCO, where the closed form cancels catastrophically in float32.
        bracket = delta * delta * (0.8164966f + delta * (-0.7777778f + delta * 0.8164966f));
    } else {
        // 0.17157288 = (sqrt6 - sqrt3) / (sqrt6 + sqrt3)
        bracket = delta + 0.8660254f * log((x + 1.7320508f) / (x - 1.7320508f) * 0.17157288f);
    }
    return 0.11936621f * bracket / ((radius - 3.0f) * radius * radius * x);
}

// Zero-mean structure field: one broad, low-contrast octave of clumps and
// ridged filaments above it, each fading out once it is smaller than the
// pixel footprint.
float diskPattern(float3 p, float footprint, uint seed) {
    float sum = 0.0f;
    float amplitude = 0.5f;
    float frequency = 1.0f;
    for (int octave = 0; octave < 6; ++octave) {
        float fade = 1.0f - smoothstep(0.3f, 1.0f, footprint * frequency);
        if (fade <= 0.0f) {
            break;
        }
        float n = valueNoise(p * frequency, seed + uint(octave) * 101u);
        float ridge = 1.0f - abs(n);
        float value = octave < 1 ? n * 0.55f : (ridge * ridge - 0.4f) * 1.7f;
        sum += amplitude * fade * value;
        amplitude *= 0.6f;
        frequency *= 2.03f;
    }
    return sum;
}

// Procedural emissivity fluctuation in roughly [-1, 1], advected by Keplerian
// differential rotation.  Illustrative, not a turbulence model.
//
// The pattern lives in (azimuth, ln r) with a trailing logarithmic-spiral
// pitch.  Advecting it forever would wind it into ever finer rings, so each
// field only lives for a bounded sweep and is cross-faded with a second one
// half a lifetime out of phase.  Because the orbital period grows as r^(3/2),
// the lifetime is chosen per logarithmic radial band (two overlapping bands
// are blended), which keeps the accumulated shear the same at every radius.
float diskTurbulence(float radius, float2 xy, float footprint) {
    static const float BAND = 0.55f;         // band spacing in ln r
    static const float SWEEP = 2.6f;         // radians a band centre turns per lifetime
    static const float PITCH = 2.4f;         // trailing winding, radians per e-fold
    static const float AZIMUTHAL = 1.7f;     // lattice cells per radian
    static const float RADIAL = 6.5f;        // lattice cells per e-fold of radius

    float log_radius = log(radius);
    float omega = exp(-1.5f * log_radius);
    float2 unit = xy / radius;
    float band_coordinate = log_radius / BAND;
    float band_base = floor(band_coordinate);
    float band_blend = smoothstep(0.0f, 1.0f, band_coordinate - band_base);

    // Lattice cells crossed per e-fold of radius, including the winding.
    float winding = AZIMUTHAL * (PITCH + 0.75f * SWEEP);
    float noise_footprint = footprint / radius * sqrt(RADIAL * RADIAL + winding * winding);

    float sum = 0.0f;
    float weight_sum_sq = 0.0f;
    for (int band_offset = 0; band_offset < 2; ++band_offset) {
        float band = band_base + float(band_offset);
        float band_weight = band_offset == 0 ? 1.0f - band_blend : band_blend;
        float lifetime = SWEEP * exp(1.5f * (band + 0.5f) * BAND);
        for (int layer = 0; layer < 2; ++layer) {
            // The golden-ratio offset keeps neighbouring bands out of step.
            float phase = frame.camera_position.w / lifetime + 0.5f * float(layer) +
                          frac(band * 0.61803399f);
            float cycle_index = floor(phase);
            float local = phase - cycle_index;
            float weight = band_weight * (1.0f - abs(2.0f * local - 1.0f));
            // Rotating the lookup by +angle moves the pattern forward by it;
            // the pitch term makes outer parts of a feature trail.
            float angle = omega * (local - 0.5f) * lifetime - PITCH * log_radius;
            float s = sin(angle);
            float c = cos(angle);
            float2 rotated = float2(unit.x * c + unit.y * s, unit.y * c - unit.x * s);
            float3 p = float3(rotated * AZIMUTHAL, log_radius * RADIAL);
            uint seed = (uint(int(cycle_index) & 0xFFFF) * 4u + uint(layer) * 2u +
                         uint(int(band) & 0xFF) * 131u + 7u) * 2654435761u;
            sum += weight * diskPattern(p, noise_footprint, seed);
            weight_sum_sq += weight * weight;
        }
    }
    // Normalising by the root sum of squares keeps the contrast constant
    // through the cross-fades of independent fields.
    return clamp(sum / sqrt(max(weight_sum_sq, 1.0e-4f)) * 2.2f, -1.0f, 1.0f);
}

float3 diskRadiance(float radius, float structure, Trace trace) {
    float u = 1.0f / radius;
    float omega = u * sqrt(u);

    // g = sqrt(1 - 3M/r) / (lapse_camera * (1 - Omega * lambda))
    float gravity = sqrt(max(1.0f - 3.0f * u, 0.0f)) / trace.lapse;
    float doppler = 1.0f / max(1.0f - omega * trace.axial, 0.05f);
    float true_shift = gravity * doppler;
    float shift = lerp(1.0f, doppler, frame.effects.x) * lerp(1.0f, gravity, frame.effects.y);

    float flux = novikovThorneFlux(radius) * frame.disk_detail.z *
                 max(1.0f + frame.disk_detail.x * structure, 0.02f);
    float temperature = frame.disk.z * sqrt(sqrt(flux));
    float3 radiance = blackbodyRadiance(shift * temperature);

    if (frame.disk_detail.y > 0.5f) {
        // cos(emission angle) in the gas frame = g * lapse * sqrt(b^2 - lambda^2) / r
        float mu = saturate(true_shift * trace.lapse * trace.impact * trace.tilt * u);
        radiance *= (1.0f + 2.06f * mu) * (1.0f / 2.3733333f);
    }
    return radiance;
}

// ---------------------------------------------------------------------------
// Shading
// ---------------------------------------------------------------------------

struct Footprints {
    SkyFootprint sky;      // screen-space derivatives of the sky direction
    // > 0: no derivatives are known; use this many radians per pixel about
    // the ray's own sky direction instead (the refinement pass).
    float sky_isotropic;
    float hit[MAX_HITS];   // metres-equivalent (M) of disk per pixel, per image order
};

float3 shadeTrace(Trace trace, Footprints footprints) {
    float3 color = float3(0.0f, 0.0f, 0.0f);
    float transmittance = 1.0f;
    for (int order = 0; order < MAX_HITS; ++order) {
        if (order >= trace.hit_count) {
            break;
        }
        float radius = trace.hit_radius[order];
        float structure = frame.disk_detail.x > 0.0f
                              ? diskTurbulence(radius, trace.hit_xy[order], footprints.hit[order])
                              : 0.0f;
        float alpha = diskAlpha(radius, structure);
        if (alpha > 0.0f) {
            color += transmittance * alpha * diskRadiance(radius, structure, trace);
            transmittance *= 1.0f - alpha;
        }
    }
    if (trace.termination == TERMINATION_ESCAPED && transmittance > 0.002f) {
        SkyFootprint sky = footprints.sky_isotropic > 0.0f
                               ? isotropicFootprint(trace.sky_dir, footprints.sky_isotropic)
                               : footprints.sky;
        color += transmittance * skyRadiance(trace.sky_dir, sky, trace.lapse);
    }
    return color;
}

float3 cameraRay(float2 pixel) {
    float2 ndc = float2(pixel.x * 2.0f * frame.viewport.z - 1.0f,
                        1.0f - pixel.y * 2.0f * frame.viewport.w);
    return normalize(frame.camera_forward.xyz + ndc.x * frame.camera_right.xyz +
                     ndc.y * frame.camera_up.xyz);
}

// b = L / E of a camera ray, from its direction alone (no integration).
float rayImpact(float3 direction) {
    float3 camera = frame.camera_position.xyz;
    float camera_radius = length(camera);
    float cos_alpha = dot(direction, camera / camera_radius);
    float sin_alpha = sqrt(saturate(1.0f - cos_alpha * cos_alpha));
    float lapse = sqrt(max(1.0f - 2.0f * frame.effects.z / camera_radius, 1.0e-6f));
    return sin_alpha * camera_radius / lapse;
}

// Impact parameter of the edge of the shadow: 3 sqrt(3) M, or the radius of
// the opaque sphere in the flat comparison.
float criticalImpact() {
    return frame.effects.z > 0.5f ? CRITICAL_IMPACT : 2.0f;
}

// R2 low-discrepancy offsets inside the pixel, relative to its centre.
float2 sampleJitter(int sample) {
    return frac(float2(0.5f, 0.5f) + float(sample) * float2(0.7548776662f, 0.5698402910f)) - 0.5f;
}

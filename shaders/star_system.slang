// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Deep-sky lab: a visited star of the galaxy with its planets and moons
// (examples/deep_sky_lab/stellar_systems.hpp), traced as spheres.
//
//   star     a black body of its radius and effective temperature, with a
//            linear limb darkening law;
//   bodies   lit by the star, reflecting with their albedo (Lambert);
//            a body between a surface point and the star hides the part of
//            the star's disk it covers, so eclipses have umbra and penumbra;
//            molten day sides glow as black bodies of the local temperature;
//   rings    an icy slab of uniform optical depth in the system's plane,
//            single scattering on the lit and the unlit side, shadowed by
//            the planet;
//   points   a body smaller than a pixel is drawn as the image of a point
//            carrying the light a Lambert sphere reflects at its phase.
//
// Units: kilometres relative to the camera.  Radiances arrive multiplied by
// the display scale (the galaxy's light scale and the exposure), so what is
// computed here lands on the same scale as the galaxy behind.
//
// system.star:
//   [0] xyz = centre, w = radius
//   [1] rgb = radiance at the disk centre, w = limb darkening coefficient
//   [2] rgb = illumination: the reflected radiance of a unit-albedo surface
//       facing the star is rgb / d^2 (d in km), w = body count
//   [3] w = thermal scale: a black body at T shines with
//       blackbody(T) * exp(a(T) - a_sun) * w
// system.bodies, four rows per body:
//   [0] xyz = centre, w = radius
//   [1] rgb = albedo, w = substellar temperature of a molten day side (0: none)
//   [2] x, y = inner and outer ring radius, z = ring optical depth (0: none)
//   [3] xyz = normal of the ring plane (the planet's equator)
// frame.object[6]: z = ln(luminance of the solar surface), w = image sigma (px).

#include "lib/deep_sky_frame.slang"
#include "lib/sky.slang"

[[vk::binding(1, 0)]] Texture2D<float4> blackbody_table;

struct SystemBlock {
    float4 star[4];
    float4 bodies[96];
};
[[vk::binding(2, 0)]] ConstantBuffer<SystemBlock> system;

struct VsOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

static const float MAX_RADIANCE = 6.0e4f;
// Single-scattering albedo of the ring particles times their tint: water ice,
// slightly reddened (the planet lab's Saturn-like rings: 0.55 and
// (0.95, 0.90, 0.82)).
static const float3 RING_ALBEDO = float3(0.52f, 0.495f, 0.45f);

// Distances along a unit ray from the origin to a sphere, or (-1, -1).  The
// closest approach is formed as a vector, which keeps its precision when the
// sphere is far away and small.
float2 hitSphere(float3 direction, float3 centre, float radius) {
    float along = dot(centre, direction);
    float3 miss = centre - direction * along;
    float h2 = radius * radius - dot(miss, miss);
    if (h2 < 0.0f) {
        return float2(-1.0f, -1.0f);
    }
    float h = sqrt(h2);
    if (along + h <= 0.0f) {
        return float2(-1.0f, -1.0f);
    }
    return float2(along - h, along + h);
}

float angleBetween(float3 a, float3 b) {
    return atan2(length(cross(a, b)), dot(a, b));
}

// Fraction of a disk of angular radius `a` covered by one of radius `b`
// whose centre is `d` away (small angles: plane geometry).
float coveredFraction(float a, float b, float d) {
    if (d >= a + b) {
        return 0.0f;
    }
    if (d <= abs(b - a)) {
        return b >= a ? 1.0f : (b * b) / (a * a);
    }
    float a2 = a * a;
    float b2 = b * b;
    float alpha = acos(clamp((d * d + a2 - b2) / (2.0f * d * a), -1.0f, 1.0f));
    float beta = acos(clamp((d * d + b2 - a2) / (2.0f * d * b), -1.0f, 1.0f));
    float area = a2 * (alpha - 0.5f * sin(2.0f * alpha)) + b2 * (beta - 0.5f * sin(2.0f * beta));
    return saturate(area / (PI * a2));
}

// Fraction of the star's disk seen from `p`, the other bodies in the way.
float starVisible(float3 p, int self, int count) {
    float3 to_star = system.star[0].xyz - p;
    float distance = length(to_star);
    float star_angle = asin(min(system.star[0].w / distance, 1.0f));
    float visible = 1.0f;
    for (int k = 0; k < count; ++k) {
        if (k == self) {
            continue;
        }
        float4 body = system.bodies[4 * k];
        float3 to_body = body.xyz - p;
        float d = length(to_body);
        if (d >= distance || dot(to_body, to_star) <= 0.0f) {
            continue;
        }
        float body_angle = asin(min(body.w / d, 1.0f));
        visible *= 1.0f - coveredFraction(star_angle, body_angle, angleBetween(to_body, to_star));
    }
    return visible;
}

// Lambert sphere: light reflected towards a phase angle, relative to full phase.
float lambertPhase(float phase) {
    return (sin(phase) + (PI - phase) * cos(phase)) / PI;
}

float3 thermal(float kelvin) {
    if (kelvin < 500.0f) {
        return float3(0.0f, 0.0f, 0.0f);
    }
    float4 entry = blackbodyEntry(blackbody_table, frame.radiometry, kelvin);
    return entry.rgb * exp(entry.a - frame.object[6].z) * system.star[3].w;
}

// Image of a point at `centre` carrying `light` (radiance times area, in km^2
// at the display scale) on the pixel's ray, and the weight of that image for
// a sphere of `radius`: one below half a pixel, none above a pixel and a half.
float3 pointImage(float2 pixel, float3 centre, float radius, float3 light, out float weight) {
    weight = 0.0f;
    float depth = dot(centre, frame.camera_forward.xyz);
    if (depth <= 0.0f) {
        return float3(0.0f, 0.0f, 0.0f);
    }
    float distance = length(centre);
    float pixel_angle = pixelAngle();
    float radius_px = radius / (distance * pixel_angle);
    weight = 1.0f - smoothstep(0.5f, 1.5f, radius_px);
    if (weight <= 0.0f) {
        return float3(0.0f, 0.0f, 0.0f);
    }
    float2 right_scaled = float2(dot(frame.camera_right.xyz, frame.camera_right.xyz),
                                 dot(frame.camera_up.xyz, frame.camera_up.xyz));
    float2 ndc = float2(dot(centre, frame.camera_right.xyz), dot(centre, frame.camera_up.xyz)) / (depth * right_scaled);
    float2 at = float2((ndc.x * 0.5f + 0.5f) * frame.viewport.x, (0.5f - 0.5f * ndc.y) * frame.viewport.y);
    float sigma = frame.object[6].w;
    float2 offset = pixel - at;
    float r2 = dot(offset, offset) / (sigma * sigma);
    if (r2 > 9.0f) {
        return float3(0.0f, 0.0f, 0.0f);
    }
    float sigma_km = sigma * pixel_angle * distance;
    return light * (weight * exp(-0.5f * r2) / (2.0f * PI * sigma_km * sigma_km));
}

[shader("fragment")]
float4 fsMain(VsOut input) : SV_Target {
    int count = int(system.star[2].w);
    // A different sub-pixel position each accumulated frame.
    float index = frame.quality.y;
    float2 jitter = frac(float2(0.7548777f, 0.5698403f) * (index + 1.0f)) - 0.5f;
    float2 pixel = input.pos.xy + jitter;
    float3 direction = cameraRay(pixel);

    float4 star = system.star[0];
    float best = 1.0e30f;
    int hit = -2; // -1 the star, k a body
    float2 star_hit = hitSphere(direction, star.xyz, star.w);
    if (star_hit.x > 0.0f) {
        best = star_hit.x;
        hit = -1;
    }
    for (int k = 0; k < count; ++k) {
        float4 body = system.bodies[4 * k];
        float2 t = hitSphere(direction, body.xyz, body.w);
        if (t.x > 0.0f && t.x < best) {
            best = t.x;
            hit = k;
        }
    }

    float3 radiance = float3(0.0f, 0.0f, 0.0f);
    float transmittance = 1.0f;

    // Rings in front of whatever the ray hits.
    for (int k = 0; k < count; ++k) {
        float4 ring = system.bodies[4 * k + 2];
        if (ring.z <= 0.0f) {
            continue;
        }
        float3 centre = system.bodies[4 * k].xyz;
        float3 normal = system.bodies[4 * k + 3].xyz;
        float facing = dot(direction, normal);
        if (abs(facing) < 1.0e-6f) {
            continue;
        }
        float t = dot(centre, normal) / facing;
        if (t <= 0.0f || t >= best) {
            continue;
        }
        float3 p = direction * t;
        float r = length(p - centre);
        if (r < ring.x || r > ring.y) {
            continue;
        }
        float3 to_star = star.xyz - p;
        float d2 = dot(to_star, to_star);
        float3 light = to_star * rsqrt(d2);
        float mu0 = max(abs(dot(light, normal)), 1.0e-3f);
        float mu = max(abs(facing), 1.0e-3f);
        float tau = ring.z;
        // Viewer and star on the same side: diffuse reflection of a slab;
        // opposite sides: diffuse transmission (Chandrasekhar 1960, single
        // scattering).
        bool lit_side = dot(light, normal) * facing < 0.0f;
        float geometric = lit_side
                              ? mu0 / (mu0 + mu) * (1.0f - exp(-tau * (1.0f / mu0 + 1.0f / mu)))
                              : (abs(mu0 - mu) < 1.0e-4f ? tau / mu0 * exp(-tau / mu0)
                                                          : mu0 / (mu0 - mu) * (exp(-tau / mu0) - exp(-tau / mu)));
        // Ring particles are rough ice that backscatters like Lambert spheres
        // (Dones, Cuzzi & Showalter 1993), with a small lobe of diffraction
        // forwards, around the scattering angle pi - alpha; the phase is
        // normalised over 4 pi.
        float alpha = angleBetween(-direction, light);
        float lambert = 2.0f * (sin(alpha) + (PI - alpha) * cos(alpha)) / (3.0f * PI * PI);
        float g = 0.7f;
        float cos_scattering = -cos(alpha);
        float lobe = (1.0f - g * g) / (4.0f * PI * pow(max(1.0f + g * g - 2.0f * g * cos_scattering, 1.0e-4f), 1.5f));
        float phase = 0.9f * lambert + 0.1f * lobe;
        float shadow = starVisible(p, -1, count);
        // I = E albedo phase pi factor, with the irradiance E = pi star[2] / d^2.
        radiance += transmittance * RING_ALBEDO * system.star[2].rgb *
                    (PI * PI * phase * abs(geometric) * shadow / d2);
        transmittance *= exp(-tau / mu);
    }

    float coverage = 1.0f - transmittance;
    if (hit == -1) {
        float3 p = direction * best;
        float mu = saturate(dot(normalize(p - star.xyz), -direction));
        float limb = system.star[1].w;
        float weight;
        pointImage(pixel, star.xyz, star.w, float3(0.0f, 0.0f, 0.0f), weight);
        radiance += transmittance * (1.0f - weight) * system.star[1].rgb * (1.0f - limb * (1.0f - mu));
        coverage = 1.0f - transmittance * weight;
    } else if (hit >= 0) {
        float4 body = system.bodies[4 * hit];
        float4 surface = system.bodies[4 * hit + 1];
        float3 p = direction * best;
        float3 n = normalize(p - body.xyz);
        float3 to_star = star.xyz - p;
        float d2 = dot(to_star, to_star);
        float cos_i = dot(n, to_star * rsqrt(d2));
        float3 shade = float3(0.0f, 0.0f, 0.0f);
        if (cos_i > 0.0f) {
            shade += surface.rgb * system.star[2].rgb * (cos_i / d2) * starVisible(p, hit, count);
            if (surface.w > 0.0f) {
                // A molten day side, hottest under the star (no redistribution).
                shade += thermal(surface.w * pow(cos_i, 0.25f));
            }
        }
        float weight;
        pointImage(pixel, body.xyz, body.w, float3(0.0f, 0.0f, 0.0f), weight);
        radiance += transmittance * (1.0f - weight) * shade;
        coverage = 1.0f - transmittance * weight;
    }

    // Bodies too small to resolve, as points.
    {
        float weight;
        // The star: radiance times projected area is its disk-averaged
        // radiance (1 - u/3) I0 times pi R^2.
        float3 light = system.star[1].rgb * (1.0f - system.star[1].w / 3.0f) * (PI * star.w * star.w);
        radiance += pointImage(pixel, star.xyz, star.w, light, weight);
    }
    for (int k = 0; k < count; ++k) {
        float4 body = system.bodies[4 * k];
        float3 to_star = star.xyz - body.xyz;
        float d2 = dot(to_star, to_star);
        float phase = angleBetween(to_star, -body.xyz);
        // A Lambert sphere at full phase reflects (2 pi / 3) R^2 of a facing
        // surface's radiance; times its phase law.
        float3 light = system.bodies[4 * k + 1].rgb * system.star[2].rgb *
                       (2.0f * PI / 3.0f * body.w * body.w * lambertPhase(phase) / d2);
        float weight;
        radiance += pointImage(pixel, body.xyz, body.w, light, weight);
    }
    radiance = min(radiance, float3(MAX_RADIANCE, MAX_RADIANCE, MAX_RADIANCE));
    // Premultiplied over: opaque bodies hide the galaxy behind them.
    return float4(radiance, saturate(coverage));
}

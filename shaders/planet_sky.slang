// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Deep-sky lab, planet: everything between the terrain and the camera, and
// behind it.  A full-screen pass over the terrain pass's colour and depth:
//
//   atmosphere   single scattering of sunlight by molecules (Rayleigh, with
//                its 1/lambda^4 colour) and aerosols (Mie, with a forward
//                lobe), integrated along the view ray through an atmosphere
//                whose densities fall exponentially with height (Nishita et
//                al. 1993; the formulation of O'Neil 2005).  The sun's
//                transmittance to each sample comes from a table of optical
//                depth over height and sun elevation, computed on the CPU for
//                the body.  The same integral gives the aerial perspective of
//                the terrain.  Multiple scattering and ozone absorption are
//                not included (Bruneton & Neyret 2008 add them), so twilight
//                is less purple and the shadowed sky a little too dark.
//   clouds       a layer of water cloud between two heights, its density
//                from a coverage field times a shape field with a vertical
//                profile (the approach of Schneider & Vos 2015 and Hillaire
//                2016), lit by marching towards the sun, with the
//                multiple-scattering approximation of Hillaire (a sum of
//                attenuated octaves) and the Henyey-Greenstein phase function.
//   rings        a flat annulus in the equatorial plane with a radial profile
//                of normal optical depth, shaded by single scattering in a
//                plane-parallel layer (Chandrasekhar 1960): reflected on the
//                lit side, transmitted on the unlit side, with the planet's
//                shadow across it.
//   sun          a disc of the sun's angular size with limb darkening, whose
//                radiance follows from the irradiance the terrain is lit
//                with; the glare pass of the post chain spreads it.
//   stars        the lab's sky, scaled to the sun's units and seen through
//                the atmosphere.
//
// Rows of frame.object:
//   [0] xyz = body centre relative to the camera (m), w = camera distance
//       from the centre (m)
//   [1] x = planet radius (m), y = atmosphere top radius (m), z = camera
//       height above the planet radius (m; precise), w = atmosphere on (0/1)
//   [2] xyz = direction to the sun (body frame), w = its angular radius (rad)
//   [3] xyz = Rayleigh scattering coefficient at the surface (1/m),
//       w = Rayleigh scale height (m)
//   [4] xyz = Mie scattering coefficient (1/m), w = Mie scale height (m)
//   [5] x = Mie asymmetry g, y = sun irradiance (the terrain's unit),
//       z = star scale, w = height the transmittance table spans (m)
//   [6] x = ring inner radius (m), y = ring outer radius (m), z = ring
//       albedo, w = rings on (0/1)
//   [7] x = cloud base radius (m), y = cloud top radius (m), z = cloud
//       cover, w = cloud extinction of dense cloud (1/m); zero = no clouds
// frame.quality.x = atmosphere samples along the ray, y = index of the
// accumulated frame (the jitter), z = cloud samples.
// frame.sky.z = drift of the clouds (rad of longitude: time times wind),
// w = 1 for a gas giant: its "surface" is the analytic sphere, so the
// atmosphere ends there rather than at the flat patches' depth.

#include "lib/deep_sky.slang"
#include "lib/planet_noise.slang"

[[vk::binding(2, 0)]] ConstantBuffer<PlanetBodyParams> body;
// The terrain pass: reversed depth (near / distance along the forward axis)
// and linear HDR radiance.
[[vk::binding(3, 0)]] Texture2D<float> terrain_depth;
[[vk::binding(4, 0)]] Texture2D<float4> terrain_color;
// Optical depth to space (r = molecules, g = aerosols, in metres of
// surface-density path) over u = height / span, v = (sun elevation cosine
// + 1) / 2; 1e9 where the sun is below the ground.
[[vk::binding(5, 0)]] Sampler2D<float4> transmittance_lut;
// Normal optical depth of the rings from the inner to the outer edge.
[[vk::binding(6, 0)]] Sampler2D<float4> ring_profile;
// The ring's tint (rgb) and the planet's mean radius for shadows (w).
[[vk::binding(7, 0)]] ConstantBuffer<PlanetSkyExtra> extra;
// Multiply scattered light per unit scattering coefficient over height (u)
// and sun elevation (v), rgb (Hillaire 2020): what the single-scattering
// march leaves out.  Built on the CPU with the transmittance table.
[[vk::binding(8, 0)]] Sampler2D<float4> multiscatter_lut;

struct PlanetSkyExtra {
    float4 ring_tint; // rgb = particle colour, w = unused
};

static const float NEAR_PLANE = 0.25f;

// Height above the planet's radius of the point at distance t along the
// ray, computed as (r^2 - R^2) / (r + R) so that the metres survive the
// million-metre radius in float32.
float heightAlong(float t, float dot_dc, float alt, float radius) {
    float f = alt * (2.0f * radius + alt) + t * t - 2.0f * t * dot_dc;
    return f / (sqrt(max(radius * radius + f, 0.0f)) + radius);
}

// Entry and exit distances of the ray through the sphere of radius r about
// the body centre; false when missed.  `alt` is the camera's height above
// the planet radius, so that the discriminant keeps its precision.
bool raySphere(float dot_dc, float alt, float radius, float r, out float t0, out float t1) {
    float camera_minus_r = alt + (radius - r); // |c| - r
    float camera_plus_r = alt + radius + r;    // |c| + r
    float discriminant = dot_dc * dot_dc - camera_minus_r * camera_plus_r;
    t0 = 0.0f;
    t1 = 0.0f;
    if (discriminant < 0.0f) {
        return false;
    }
    float root = sqrt(discriminant);
    t0 = dot_dc - root;
    t1 = dot_dc + root;
    return t1 > 0.0f;
}

float rayleighPhase(float cos_theta) {
    return 3.0f / (16.0f * PI) * (1.0f + cos_theta * cos_theta);
}

// Cornette-Shanks phase function for aerosols.
float miePhase(float cos_theta, float g) {
    float g2 = g * g;
    float denominator = 1.0f + g2 - 2.0f * g * cos_theta;
    return 3.0f / (8.0f * PI) * (1.0f - g2) * (1.0f + cos_theta * cos_theta) /
           ((2.0f + g2) * denominator * sqrt(denominator));
}

float henyeyGreenstein(float cos_theta, float g) {
    float g2 = g * g;
    float denominator = 1.0f + g2 - 2.0f * g * cos_theta;
    return (1.0f - g2) / (4.0f * PI * denominator * sqrt(denominator));
}

float2 opticalDepthToSpace(float height, float mu) {
    float span = max(frame.object[5].w, 1.0f);
    float2 uv = float2(saturate(height / span), saturate(mu * 0.5f + 0.5f));
    return transmittance_lut.Sample(uv).rg;
}

float3 multipleScattering(float height, float mu) {
    float span = max(frame.object[5].w, 1.0f);
    float2 uv = float2(saturate(height / span), saturate(mu * 0.5f + 0.5f));
    return multiscatter_lut.Sample(uv).rgb;
}

// ---------------------------------------------------------------------------
// Atmosphere
// ---------------------------------------------------------------------------

struct Scattering {
    float3 inscatter;     // radiance scattered towards the camera
    float3 transmittance; // of the segment
};

// Single scattering along the ray between distances `start` and `end`.
Scattering atmosphereSegment(float3 direction, float dot_dc, float alt, float radius, float start,
                             float end, int samples, float jitter) {
    Scattering result;
    result.inscatter = float3(0.0f, 0.0f, 0.0f);
    result.transmittance = float3(1.0f, 1.0f, 1.0f);
    if (end <= start || samples <= 0) {
        return result;
    }
    float3 centre = frame.object[0].xyz;
    float3 to_sun = frame.object[2].xyz;
    float3 beta_r = frame.object[3].xyz;
    float h_r = frame.object[3].w;
    float3 beta_m = frame.object[4].xyz;
    float h_m = frame.object[4].w;
    float g = frame.object[5].x;
    float irradiance = frame.object[5].y;
    float cos_theta = dot(direction, to_sun);
    float phase_r = rayleighPhase(cos_theta);
    float phase_m = miePhase(cos_theta, g);
    float ds = (end - start) / float(samples);
    float2 depth_camera = float2(0.0f, 0.0f); // molecules, aerosols
    float3 inscatter = float3(0.0f, 0.0f, 0.0f);
    for (int i = 0; i < samples; ++i) {
        float t = start + (float(i) + jitter) * ds;
        float height = max(heightAlong(t, dot_dc, alt, radius), 0.0f);
        float2 density = float2(exp(-height / h_r), exp(-height / h_m));
        float3 position = direction * t - centre; // from the body centre
        float mu = dot(position, to_sun) / max(length(position), 1.0f);
        float2 depth_sun = opticalDepthToSpace(height, mu);
        float2 depth_view = depth_camera + 0.5f * density * ds;
        float3 view_attenuation = exp(-(beta_r * depth_view.x + beta_m * depth_view.y));
        float3 sun_attenuation = exp(-(beta_r * depth_sun.x + beta_m * depth_sun.y));
        // Sunlight scattered once towards the camera, and the light that
        // has already scattered at least once (isotropic by then).
        float3 single = sun_attenuation * (beta_r * (density.x * phase_r) + beta_m * (density.y * phase_m));
        float3 multiple = (beta_r * density.x + beta_m * density.y) * multipleScattering(height, mu);
        inscatter += view_attenuation * (single + multiple) * ds;
        depth_camera += density * ds;
    }
    result.transmittance = exp(-(beta_r * depth_camera.x + beta_m * depth_camera.y));
    result.inscatter = inscatter * irradiance;
    return result;
}

// Sky radiance near the zenith at low altitude: the ambient light of the
// clouds, from the same single scattering with the table.
float3 zenithSky() {
    float3 beta_r = frame.object[3].xyz;
    float3 beta_m = frame.object[4].xyz;
    float3 to_sun = frame.object[2].xyz;
    float mu = saturate(dot(to_sun, -normalize(frame.object[0].xyz)));
    float2 depth_sun = opticalDepthToSpace(0.0f, mu);
    float3 sun = exp(-(beta_r * depth_sun.x + beta_m * depth_sun.y));
    float3 column = beta_r * frame.object[3].w + beta_m * frame.object[4].w;
    return ((beta_r * frame.object[3].w * 0.08f + beta_m * frame.object[4].w * 0.05f) * sun +
            column * multipleScattering(0.0f, mu)) *
           frame.object[5].y;
}

// ---------------------------------------------------------------------------
// Clouds
// ---------------------------------------------------------------------------

// Density of cloud at a point (relative to the body centre), 0..1, given
// the cover of the sky there (cloudCoverage of its direction).  `detail`
// (1 close, 0 far) fades the billows out as they fall below the pixel
// footprint, leaving their statistics.
float cloudDensity(float3 position, float detail, float coverage) {
    float r = length(position);
    float base = frame.object[7].x;
    float top = frame.object[7].y;
    float u = (r - base) / max(top - base, 1.0f);
    if (u <= 0.0f || u >= 1.0f || coverage <= 0.001f) {
        return 0.0f;
    }
    // Shape: billows a few kilometres across, eroded by finer detail.
    float shape = 0.5f;
    if (detail > 0.0f) {
        float3 p = position * (1.0f / 2500.0f);
        float billows = planetValueNoiseD(int3(0, 0, 0), p, 0x9E3779B9u, 0u).x * 0.6f +
                        planetValueNoiseD(int3(0, 0, 0), p * 2.3f, 0x9E3779B9u, 1u).x * 0.3f +
                        planetValueNoiseD(int3(0, 0, 0), p * 5.1f, 0x9E3779B9u, 2u).x * 0.1f;
        // The noise is signed; half its swing keeps the billows' spread near
        // 0.15, the width the far-field statistics assume.
        shape = 0.5f + 0.5f * lerp(0.0f, billows, detail);
    }
    // Cumulus profile: flat bottoms, rounded tops.
    float profile = smoothstep(0.0f, 0.12f, u) * (1.0f - smoothstep(0.45f, 1.0f, u));
    // Erosion (Schneider & Vos 2015): cloud where the shape exceeds what
    // the coverage leaves of it.
    // A steep edge: a billow is either there or not, over a tenth of the
    // shape's range.
    float resolved = saturate((shape - (1.0f - coverage)) * 6.0f);
    // Below the pixel footprint the billows average to a fraction of
    // opaque cloud: the density that gives their mean transmittance.
    float layer = max(top - base, 1.0f);
    float far = cloudFarOpticalDepth(coverage) / max(frame.object[7].w * layer * 0.6f, 1.0e-4f);
    return lerp(far, resolved, detail) * profile;
}

// Reflectance of a conservative slab of optical depth `tau` with the
// asymmetry of water droplets (g = 0.85) in the hemispheric-mean two-stream
// approximation (Meador & Weaver 1980): gamma tau / (1 + gamma tau) with
// gamma = 3 (1 - g) / 4.  Thick stratocumulus (tau 20 to 40) reflects 60 to
// 75 % of sunlight, as observed.
float cloudSlabReflectance(float tau) {
    float gamma = 0.75f * (1.0f - 0.85f);
    return gamma * tau / (1.0f + gamma * tau);
}

struct CloudResult {
    float3 radiance;
    float transmittance;
    float distance; // to the first dense sample (for the atmosphere split)
};

CloudResult cloudSegment(float3 direction, float start, float end, int samples, float jitter) {
    CloudResult result;
    result.radiance = float3(0.0f, 0.0f, 0.0f);
    result.transmittance = 1.0f;
    result.distance = start;
    if (end <= start || samples <= 0) {
        return result;
    }
    float3 centre = frame.object[0].xyz;
    float3 to_sun = frame.object[2].xyz;
    float extinction = frame.object[7].w;
    float irradiance = frame.object[5].y;
    float layer = frame.object[7].y - frame.object[7].x;
    float cos_theta = dot(direction, to_sun);
    // Forward and back lobes of water droplets.
    float phase = 0.8f * henyeyGreenstein(cos_theta, 0.6f) + 0.2f * henyeyGreenstein(cos_theta, -0.2f);
    // The pixel's angular size: billows (2.5 km) below it average out.
    float pixel_angle = 2.0f * length(frame.camera_up.xyz) / max(frame.viewport.y, 1.0f);
    // The sky above and the ground below light the cloud diffusely.
    float3 ambient = zenithSky() * 0.6f + irradiance * 0.03f;
    float ds = (end - start) / float(samples);
    float light_step = layer / 3.0f;
    bool found = false;
    // The cover is a field on the sphere of directions, and a ray's direction
    // from the centre changes slowly through the thin layer: it is evaluated
    // again only once the direction has moved a tenth of a degree (the
    // field's finest cells are 0.15 degrees), and the light samples share it.
    float3 cover_direction = float3(0.0f, 0.0f, 0.0f);
    float coverage = 0.0f;
    for (int i = 0; i < samples && result.transmittance > 0.01f; ++i) {
        float t = start + (float(i) + jitter) * ds;
        float3 position = direction * t - centre;
        float3 sample_direction = normalize(position);
        if (dot(sample_direction, cover_direction) < 0.9999985f) {
            cover_direction = sample_direction;
            coverage = cloudCoverage(sample_direction, frame.sky.z, frame.object[7].z);
        }
        // Billows (2.5 km) are resolved while the pixel footprint is well
        // below their size.
        float detail = saturate(1.0f - t * pixel_angle / 1200.0f);
        float density = cloudDensity(position, detail, coverage);
        if (density <= 0.0f) {
            continue;
        }
        if (!found) {
            result.distance = t;
            found = true;
        }
        float r = length(position);
        float mu0 = saturate(dot(position, to_sun) / r);
        // The column of a billow: dense cloud through half the layer.
        float tau_column = extinction * layer * 0.3f;
        // Optical depth towards the sun through the billows while they
        // resolve; afar, through the billow above the sample.
        float depth_sun;
        if (detail > 0.0f) {
            depth_sun = 0.0f;
            for (int j = 1; j <= 3; ++j) {
                float3 sample = position + to_sun * (float(j) - 0.5f) * light_step;
                depth_sun += cloudDensity(sample, 1.0f, coverage) * light_step;
            }
            depth_sun *= extinction;
            float above = saturate((frame.object[7].y - r) / max(layer, 1.0f));
            depth_sun = lerp(tau_column * above / max(mu0, 0.15f), depth_sun, detail);
        } else {
            float above = saturate((frame.object[7].y - r) / max(layer, 1.0f));
            depth_sun = tau_column * above / max(mu0, 0.15f);
        }
        // Direct sunlight scattered once, plus the multiply scattered light
        // of the slab, which in a conservative slab falls linearly from the
        // lit face (its reflectance) to the far face (its diffuse
        // transmission): Eddington's solution.
        float reflectance = cloudSlabReflectance(tau_column);
        float transmission = (1.0f - reflectance) * (1.0f - exp(-tau_column));
        float fraction = saturate(depth_sun / max(tau_column, 1.0e-3f));
        float diffuse = lerp(reflectance, transmission, fraction);
        float3 light = irradiance * (phase * exp(-depth_sun) + mu0 * diffuse / PI) + ambient;
        float sigma = extinction * density;
        float step_transmittance = exp(-sigma * ds);
        // Integrated over the step for a constant source.
        result.radiance += result.transmittance * light * (1.0f - step_transmittance);
        result.transmittance *= step_transmittance;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Rings
// ---------------------------------------------------------------------------

struct RingResult {
    float3 radiance;
    float transmittance;
    float distance;
    bool hit;
};

RingResult rings(float3 direction) {
    RingResult result;
    result.radiance = float3(0.0f, 0.0f, 0.0f);
    result.transmittance = 1.0f;
    result.distance = 1.0e30f;
    result.hit = false;
    if (frame.object[6].w < 0.5f) {
        return result;
    }
    float3 centre = frame.object[0].xyz;
    float3 camera = -centre; // from the body centre
    if (abs(direction.z) < 1.0e-6f) {
        return result;
    }
    float t = -camera.z / direction.z;
    if (t <= 0.0f) {
        return result;
    }
    float3 point = camera + direction * t;
    float r = length(point.xy);
    float inner = frame.object[6].x;
    float outer = frame.object[6].y;
    if (r < inner || r > outer) {
        return result;
    }
    float tau = ring_profile.Sample(float2((r - inner) / (outer - inner), 0.5f)).r;
    result.hit = true;
    result.distance = t;
    if (tau <= 1.0e-4f) {
        return result;
    }
    float3 to_sun = frame.object[2].xyz;
    float mu = max(abs(direction.z), 1.0e-3f);   // emission
    float mu0 = max(abs(to_sun.z), 1.0e-3f);     // incidence
    // The planet's shadow: the sun ray from the ring point enters the planet.
    float radius = frame.object[1].x;
    float along = dot(point, to_sun);
    float3 across = point - to_sun * along;
    bool shadowed = along < 0.0f && dot(across, across) < radius * radius;
    result.transmittance = exp(-tau / mu);
    if (shadowed) {
        return result;
    }
    float cos_alpha = dot(-direction, to_sun); // cosine of the scattering angle
    // Ring particles are centimetre-to-metre bodies of rough ice: they
    // backscatter like Lambert spheres, far more flatly than a
    // Henyey-Greenstein lobe (Dones, Cuzzi & Showalter 1993 fit Saturn's
    // rings with phase functions of this kind), with a small forward
    // diffraction lobe.  The Lambert sphere phase, normalised over 4 pi,
    // is 2 (sin a + (pi - a) cos a) / (3 pi^2).
    float alpha = acos(clamp(cos_alpha, -1.0f, 1.0f));
    float lambert = 2.0f * (sin(alpha) + (PI - alpha) * cos(alpha)) / (3.0f * PI * PI);
    float phase = 0.9f * lambert + 0.1f * henyeyGreenstein(cos_alpha, 0.7f);
    float albedo = frame.object[6].z;
    bool same_side = (camera.z * to_sun.z) > 0.0f;
    float factor;
    if (same_side) {
        factor = mu0 / (mu + mu0) * (1.0f - exp(-tau * (1.0f / mu + 1.0f / mu0)));
    } else if (abs(mu0 - mu) < 1.0e-3f) {
        factor = tau / mu * exp(-tau / mu);
    } else {
        factor = mu0 / (mu0 - mu) * (exp(-tau / mu0) - exp(-tau / mu));
    }
    // I / F = albedo * phase * factor * pi (phase normalised to 4 pi) -> radiance
    // per unit irradiance.
    result.radiance = extra.ring_tint.rgb * (albedo * phase * PI * max(factor, 0.0f) * frame.object[5].y);
    return result;
}

// ---------------------------------------------------------------------------

// Sub-pixel offset of this accumulated frame (Halton 2, 3): the terrain
// pass shifts its projection by the same amount, so the average of the
// frames is antialiased.
float haltonSequence(uint index, uint base) {
    float f = 1.0f;
    float r = 0.0f;
    while (index > 0u) {
        f /= float(base);
        r += f * float(index % base);
        index /= base;
    }
    return r;
}

float2 frameJitter() {
    uint index = uint(frame.quality.y) % 64u + 1u;
    return float2(haltonSequence(index, 2u), haltonSequence(index, 3u)) - 0.5f;
}

[shader("fragment")]
float4 fsMain(VsOut input) : SV_Target {
    float2 pixel = input.pos.xy;
    int2 texel = int2(pixel);
    float3 direction = cameraRay(pixel + frameJitter());
    float3 centre = frame.object[0].xyz;
    float radius = frame.object[1].x;
    float top = frame.object[1].y;
    float alt = frame.object[1].z;
    // A camera exactly on the top of the atmosphere (the descent route's
    // 100 km waypoint) makes the sphere test degenerate; stand just inside.
    if (abs(alt - top) < 5.0f) {
        alt = top - 5.0f;
    }
    float3 to_sun = frame.object[2].xyz;
    float sun_radius = frame.object[2].w;
    float irradiance = frame.object[5].y;
    float dot_dc = dot(direction, centre);
    float jitter = marchJitter(pixel);

    // Where the terrain is, along this ray.
    float depth = terrain_depth.Load(int3(texel, 0));
    float3 terrain = terrain_color.Load(int3(texel, 0)).rgb;
    bool hit = depth > 0.0f;
    float cos_forward = max(dot(direction, frame.camera_forward.xyz), 1.0e-4f);
    float t_hit = hit ? (NEAR_PLANE / depth) / cos_forward : 1.0e30f;
    if (hit && frame.sky.w > 0.5f) {
        // A gas giant has no relief: the patches only approximate its sphere,
        // and the thick air above would show their facets.
        float s0;
        float s1;
        if (raySphere(dot_dc, alt, radius, radius, s0, s1) && s0 > 0.0f) {
            t_hit = s0;
        }
    } else if (hit && body.color_ocean_deep.w > 0.5f) {
        // The ocean is exactly the sphere at the sea level; the flat patches
        // that draw it lie inside that sphere by their chord error, which
        // the air above would show as a faint grid.  Nothing lies beyond the
        // sea surface, so the air ends there at the latest.
        float s0;
        float s1;
        if (raySphere(dot_dc, alt, radius, radius + body.shape.y, s0, s1) && s0 > 0.0f) {
            t_hit = min(t_hit, s0);
        }
    }

    // The rings, if the ray crosses them before the terrain.
    RingResult ring = rings(direction);
    bool ring_in_front = ring.hit && ring.distance < t_hit;

    // The background: the terrain, or the sun and the stars.
    float3 background = float3(0.0f, 0.0f, 0.0f);
    if (hit) {
        background = terrain;
    } else {
        float cos_sun = dot(direction, to_sun);
        float angle = acos(clamp(cos_sun, -1.0f, 1.0f));
        if (angle < sun_radius) {
            float x = angle / sun_radius;
            float limb = 1.0f - 0.6f * (1.0f - sqrt(max(1.0f - x * x, 0.0f)));
            background += irradiance / (PI * sun_radius * sun_radius) * limb;
        }
        background += backgroundSky(direction) * frame.object[5].z;
    }

    // The atmosphere, split around the clouds.
    float3 color = background;
    if (frame.object[1].w > 0.5f) {
        float t0;
        float t1;
        if (raySphere(dot_dc, alt, radius, top, t0, t1)) {
            float start = max(t0, 0.0f);
            float end = min(t1, t_hit);
            if (ring_in_front) {
                end = min(end, ring.distance);
            }
            int samples = clamp(int(frame.quality.x), 4, 64);
            // Clouds: the part of the ray inside the layer and before the terrain.
            CloudResult cloud;
            cloud.transmittance = 1.0f;
            cloud.radiance = float3(0.0f, 0.0f, 0.0f);
            cloud.distance = end;
            if (frame.object[7].w > 0.0f) {
                float c0;
                float c1;
                float b0;
                float b1;
                bool outer = raySphere(dot_dc, alt, radius, frame.object[7].y, c0, c1);
                bool inner = raySphere(dot_dc, alt, radius, frame.object[7].x, b0, b1);
                if (outer) {
                    // Inside the shell between the two spheres: from the outer
                    // entry to the inner entry (or the outer exit), and, when the
                    // camera is below, from the inner exit to the outer exit.
                    float s0 = max(c0, 0.0f);
                    float s1 = inner && b0 > 0.0f ? b0 : c1;
                    if (inner && b0 <= 0.0f) {
                        // Camera below the layer: the segment starts at the inner exit.
                        s0 = max(b1, 0.0f);
                        s1 = c1;
                    }
                    s1 = min(s1, end);
                    if (s1 > s0) {
                        int cloud_samples = clamp(int(frame.quality.z), 4, 96);
                        cloud = cloudSegment(direction, s0, s1, cloud_samples, jitter);
                    }
                }
            }
            if (cloud.transmittance < 1.0f) {
                float split = clamp(cloud.distance, start, end);
                Scattering near = atmosphereSegment(direction, dot_dc, alt, radius, start, split,
                                                    max(samples / 2, 2), jitter);
                Scattering far = atmosphereSegment(direction, dot_dc, alt, radius, split, end,
                                                   max(samples / 2, 2), jitter);
                float3 behind = far.inscatter + far.transmittance * background;
                color = near.inscatter +
                        near.transmittance * (cloud.radiance + cloud.transmittance * behind);
            } else {
                Scattering all = atmosphereSegment(direction, dot_dc, alt, radius, start, end, samples, jitter);
                color = all.inscatter + all.transmittance * background;
            }
        }
    }

    // The rings in front of everything (outside the atmosphere), or behind
    // the atmosphere and in front of the background.
    if (ring_in_front) {
        float t0;
        float t1;
        bool through_air = frame.object[1].w > 0.5f && raySphere(dot_dc, alt, radius, top, t0, t1) &&
                           ring.distance > max(t0, 0.0f);
        if (through_air) {
            // Seen from inside or beyond the atmosphere: it sits behind the air.
            color = color * ring.transmittance + ring.radiance;
        } else {
            color = ring.radiance + ring.transmittance * color;
        }
    }
    return float4(color, 1.0f);
}

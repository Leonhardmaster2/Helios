// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Deep-sky lab: the galaxy, drawn from its simulated particles.
//
// Every instance is one star particle of the N-body run
// (examples/deep_sky_lab/galaxy_simulation): tens of thousands of solar
// masses of stars with a common birth time.  Nothing about spiral arms, bars
// or dust lanes is known to this shader.  Per particle it computes
//
//   light   mass times the light of a stellar population of the particle's
//           age (examples/deep_sky_lab/stellar_population): blue and bright
//           when young, yellow-orange and faint when old;
//   image   a Gaussian whose width is the distance over which the particle's
//           stars are spread: the local spacing between particles for an old
//           population, a few parsecs for a young cluster that has not had
//           time to disperse, never less than the pixel;
//   dust    extinction and reddening by the gas between the particle and the
//           camera, integrated through the simulated gas layer with the
//           Milky Way's dust-to-gas ratio.  Stars younger than 10 Myr are
//           still inside the cloud they formed in, which the mesh does not
//           resolve; they get the birth-cloud optical depth of Charlot &
//           Fall (2000).
//
// Old populations are smooth and by far the most numerous, so their light is
// drawn into a half-resolution target that the background pass adds to the
// sky; only populations younger than 50 Myr, whose images are sharp, are drawn
// at full resolution.
//
// A further pass draws the gas that particles younger than a few Myr keep
// ionised: a Stromgren sphere per particle, whose size follows from the
// ionising photon rate and the gas density (at least that of a molecular
// cloud complex, since that is where the stars formed), radiating the
// recombination and forbidden lines.
//
// Units: kiloparsec, megayear, solar mass.  frame.object:
//   [0] x = simulation time (Myr), y = disk particle count,
//       z = bulge particle count, w = pass (0 light of young stars,
//       1 ionised gas, 2 light of old stars)
//   [1] x = disk particle mass, y = bulge particle mass,
//       z = cloud particle mass, w = radiance per (solar luminosity / pc^2)
//   [2] x = 1 / (2 * half extent of the maps), y = bulge scale radius,
//       z = bulge mass, w = visual optical depth per (solar mass / pc^2)
//   [3] x = ln(minimum age) of the population table, y = 1 / ln(max / min),
//       z = table texel count, w = smallest image sigma (px)
//   [4] rgb = light of ionised gas per unit ionising rate, w = kernel width
//       in mean particle spacings
//   [5] x = position at code 1 (kpc), y = distance per Myr at velocity code 1
//       (kpc), z = time since the drawn state was published (Myr)
//
// Near the camera the brightest stars of a particle are drawn one by one
// (galaxy_resolved_stars.slang); a third attribute from a second buffer is
// the fraction of its light left to its image here, and the image fades out
// when the camera is inside it.
//
// Per instance (shaders/abi.hpp GalaxyParticle): position and birth-time code
// as normalised uint16, velocity as normalised int16.  The simulation
// publishes states a few times per tenth of a second; between them each
// particle is carried along its velocity, so the image moves smoothly at any
// frame rate.

#include "lib/deep_sky_frame.slang"

// rgb = light per solar mass formed (solar luminosities), a = ionising
// photon rate per solar mass formed (1e46 / s); log-spaced in age.
[[vk::binding(1, 0)]] Texture2D<float4> population_table;
// r = gas surface density, b = stellar surface density (solar masses / pc^2),
// g = scale height of the gas layer (kpc), over the mesh of the simulation.
[[vk::binding(2, 0)]] Sampler2D galaxy_maps;

// Birth-time code (see shaders/abi.hpp).
static const float BIRTH_OLDEST_MYR = -13000.0f;
static const float BIRTH_RECENT_MYR = -100.0f;
static const float BIRTH_LATEST_MYR = 16000.0f;
static const float BIRTH_SPLIT = 16383.0f;
static const float BIRTH_GAS = 65535.0f;       // still gas: emits no starlight
static const float SQRT_TWO_PI = 2.5066283f;
static const float MAX_SIGMA_PX = 48.0f;
// Populations younger than this are drawn at full resolution.
static const float YOUNG_MYR = 50.0f;
// After this age a population has no stars left that ionise hydrogen.
static const float IONISING_MYR = 60.0f;
// Images are cut off at this many sigma; the profile is lowered to reach zero
// there and rescaled to keep its integral.
static const float GAUSSIAN_EXTENT = 2.5f;
static const float GAUSSIAN_EDGE = 0.0439369f;  // exp(-GAUSSIAN_EXTENT^2 / 2)
static const float GAUSSIAN_SCALE = 1.2212f;
// An unbound young association expands at about 3 km/s = 3.07e-3 kpc / Myr.
static const float DISPERSAL_KPC_PER_MYR = 3.07e-3f;
static const float CLUSTER_RADIUS_KPC = 0.004f;
// Hydrogen atoms per cm^3 in gas of one solar mass per pc^3 (71 % hydrogen).
static const float ATOMS_PER_SOLAR_PC3 = 28.7f;
// Stromgren radius in parsec for 1e46 ionising photons per second in gas of
// one atom per cm^3 (case B recombination at 1e4 K).
static const float STROMGREN_PC = 6.795f;
// Mean density of the cloud complex around a newborn cluster, atoms per cm^3.
static const float BIRTH_CLOUD_ATOMS = 10.0f;
// Charlot & Fall (2000): visual optical depth of the birth cloud, which
// disperses after 10 Myr.
static const float BIRTH_CLOUD_DEPTH = 1.0f;
static const float BIRTH_CLOUD_MYR = 10.0f;

struct StarVsOut {
    float4 pos : SV_Position;
    // Offset from the centre in units of the profile's scale.
    float2 offset : TEXCOORD0;
    // rgb = radiance at the centre, a = profile (0 Gaussian, 1 sphere).
    nointerpolation float4 light : TEXCOORD1;
};

float4 mapsAt(float2 xy) {
    return galaxy_maps.SampleLevel(xy * frame.object[2].x + 0.5f, 0.0f);
}

float4 populationAt(float age_myr) {
    float count = frame.object[3].z;
    float position = clamp((log(max(age_myr, 1.0e-3f)) - frame.object[3].x) * frame.object[3].y,
                           0.0f, 1.0f) * (count - 1.0f);
    float base = min(floor(position), count - 2.0f);
    float4 lower = population_table.Load(int3(int(base), 0, 0));
    float4 upper = population_table.Load(int3(int(base) + 1, 0, 0));
    return lerp(lower, upper, position - base);
}

// Gas column between `position` and the camera, in solar masses per pc^2.
// The gas layer is Gaussian in height with the local hydrostatic thickness;
// the path is sampled where it crosses the layer.
float gasColumn(float3 position, float3 to_camera) {
    static const int SAMPLES = 6;
    static const float MAX_PATH = 12.0f; // kpc; bounds near edge-on sight lines
    float thickness = mapsAt(position.xy).g;
    float direction = to_camera.z >= 0.0f ? 1.0f : -1.0f;
    float slope = direction * max(abs(to_camera.z), 0.02f);
    float exit_height = direction * 3.0f * thickness;
    // A star on the camera's side of the layer has nothing in front of it.
    if ((position.z - exit_height) * direction >= 0.0f) {
        return 0.0f;
    }
    float entry_height = (position.z + exit_height) * direction < 0.0f ? -exit_height : position.z;
    float climb = exit_height - entry_height;
    climb = direction * min(abs(climb), MAX_PATH * abs(slope));
    float column = 0.0f;
    for (int index = 0; index < SAMPLES; ++index) {
        float height = entry_height + climb * ((float(index) + 0.5f) / float(SAMPLES));
        float along = (height - position.z) / slope;
        float4 gas = mapsAt(position.xy + to_camera.xy * along);
        float scaled = height / gas.g;
        column += gas.r * exp(-0.5f * scaled * scaled) / (SQRT_TWO_PI * gas.g);
    }
    return column * abs(climb) / (float(SAMPLES) * abs(slope));
}

[shader("vertex")]
StarVsOut vsMain(float4 star : ATTRIB0, float4 motion : ATTRIB1, float unresolved : ATTRIB2,
                 uint vertex_id : SV_VulkanVertexID, uint instance_id : SV_VulkanInstanceID) {
    StarVsOut output;
    output.pos = float4(2.0f, 2.0f, 0.0f, 1.0f); // outside the clip volume
    output.offset = float2(0.0f, 0.0f);
    output.light = float4(0.0f, 0.0f, 0.0f, 0.0f);

    float code = star.w * 65535.0f;
    float birth = code <= BIRTH_SPLIT
                      ? lerp(BIRTH_OLDEST_MYR, BIRTH_RECENT_MYR, code / BIRTH_SPLIT)
                      : lerp(BIRTH_RECENT_MYR, BIRTH_LATEST_MYR,
                             (code - BIRTH_SPLIT - 1.0f) / (65534.0f - BIRTH_SPLIT - 1.0f));
    float3 position = (star.xyz * 2.0f - 1.0f) * frame.object[5].x +
                      motion.xyz * (frame.object[5].y * frame.object[5].z);
    float age = frame.object[0].x - birth;
    float3 relative = position - frame.camera_position.xyz;
    float depth = dot(relative, frame.camera_forward.xyz);
    if (code > BIRTH_GAS - 0.5f || depth <= 1.0e-3f) {
        return output;
    }
    // Most particles belong to one pass only; leave the others before any
    // texture is read.
    int pass = int(frame.object[0].w + 0.5f);
    bool ionised_pass = pass == 1;
    bool young = age < YOUNG_MYR;
    if ((pass == 0 && !young) || (pass == 2 && young) || (ionised_pass && age > IONISING_MYR)) {
        return output;
    }

    float disk_count = frame.object[0].y;
    float bulge_count = frame.object[0].z;
    float index = float(instance_id);
    bool is_bulge = index >= disk_count && index < disk_count + bulge_count;
    float mass = index < disk_count ? frame.object[1].x
                                    : (is_bulge ? frame.object[1].y : frame.object[1].z);
    float4 population = populationAt(age);
    float4 maps = mapsAt(position.xy);
    float kiloparsec_per_pixel = depth * pixelAngle();

    float sigma_px;
    float extent;      // quad half-size in units of sigma_px
    float3 radiance;   // at the centre, solar luminosities per pc^2
    if (!ionised_pass) {
        // Distance over which the particle's stars are spread.
        float spread;
        if (is_bulge) {
            // Mean spacing in a Hernquist sphere of the bulge's mass.
            float scale = frame.object[2].y;
            float radius = max(length(position), 0.02f * scale);
            float sum = radius + scale;
            float density = frame.object[2].z * scale / (6.2831853f * radius * sum * sum * sum);
            spread = frame.object[4].w * 0.6f * pow(mass / density, 1.0f / 3.0f);
        } else {
            // Mean spacing in the disk, from the stellar surface density.
            spread = frame.object[4].w * sqrt(mass / max(maps.b, 1.0f)) * 1.0e-3f;
            spread = min(spread, CLUSTER_RADIUS_KPC + DISPERSAL_KPC_PER_MYR * age);
        }
        float true_px = spread / kiloparsec_per_pixel;
        sigma_px = clamp(true_px, frame.object[3].w, MAX_SIGMA_PX);
        extent = GAUSSIAN_EXTENT;
        // An image narrower than a pixel keeps its light; one wider than the
        // largest drawn keeps its surface brightness (the light beyond the
        // drawn part is spread too thin to matter).
        float sigma_pc = max(sigma_px, true_px) * kiloparsec_per_pixel * 1000.0f;
        // The stars drawn one by one take their share of the light; from
        // inside, what is left is spread over the whole sky.
        float inside = smoothstep(0.5f, 2.0f, length(relative) / max(spread, 1.0e-6f));
        radiance = population.rgb * (mass / (6.2831853f * sigma_pc * sigma_pc)) * (unresolved * inside);
        if (unresolved * inside <= 0.0f) {
            return output;
        }
    } else {
        float rate = mass * population.a; // 1e46 photons per second
        if (rate < 50.0f) {
            return output;
        }
        float thickness_pc = maps.g * 1000.0f;
        float scaled = position.z / maps.g;
        float atoms = max(ATOMS_PER_SOLAR_PC3 * maps.r * exp(-0.5f * scaled * scaled) /
                              (SQRT_TWO_PI * thickness_pc),
                          BIRTH_CLOUD_ATOMS);
        float radius_pc = clamp(STROMGREN_PC * pow(rate / (atoms * atoms), 1.0f / 3.0f), 5.0f, 500.0f);
        float radius_px = radius_pc / (kiloparsec_per_pixel * 1000.0f);
        sigma_px = clamp(radius_px, 1.2f * frame.object[3].w, 4.0f * MAX_SIGMA_PX);
        extent = 1.0f;
        float drawn_pc = sigma_px * kiloparsec_per_pixel * 1000.0f;
        // A uniformly emitting sphere seen in projection: surface brightness
        // 3 L / (2 pi R^2) at the centre.
        radiance = frame.object[4].rgb * (rate * 3.0f / (6.2831853f * drawn_pc * drawn_pc));
    }

    float3 to_camera = -relative / length(relative);
    float column = gasColumn(position, to_camera);
    float embedded = 1.0f - smoothstep(0.8f * BIRTH_CLOUD_MYR, 1.2f * BIRTH_CLOUD_MYR, age);
    // Both scale with the dust-to-gas ratio (object[2].w is 0.043 at the
    // Milky Way's).
    float depth_scale = frame.object[2].w;
    float optical_depth = depth_scale * column + depth_scale * (BIRTH_CLOUD_DEPTH / 0.043f) * embedded;
    float3 transmittance = exp(-DUST_EXTINCTION * optical_depth);

    float2 corner = float2(float(vertex_id & 1u), float((vertex_id >> 1u) & 1u)) * 2.0f - 1.0f;
    float2 right_scaled = float2(dot(frame.camera_right.xyz, frame.camera_right.xyz),
                                 dot(frame.camera_up.xyz, frame.camera_up.xyz));
    float2 centre = float2(dot(relative, frame.camera_right.xyz), dot(relative, frame.camera_up.xyz)) /
                    (depth * right_scaled);
    float2 half_size = extent * sigma_px * 2.0f * frame.viewport.zw;
    output.pos = float4(centre + corner * half_size, 0.0f, 1.0f);
    output.offset = corner * extent;
    output.light = float4(radiance * transmittance * frame.object[1].w, ionised_pass ? 1.0f : 0.0f);
    return output;
}

[shader("fragment")]
float4 fsMain(StarVsOut input) : SV_Target {
    float r2 = dot(input.offset, input.offset);
    float profile = input.light.a < 0.5f
                        ? GAUSSIAN_SCALE * max(exp(-0.5f * r2) - GAUSSIAN_EDGE, 0.0f)
                        : sqrt(max(1.0f - r2, 0.0f));
    return float4(input.light.rgb * profile, 0.0f);
}

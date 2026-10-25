// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Deep-sky lab gas dynamics: initial conditions.
//
// Each scenario starts from a state that contains none of the structure it is
// run to produce:
//
//   nebula   a round, uniform molecular cloud with a random turbulent
//            velocity field (the cascade it inherits from larger scales) and
//            no star: filaments and clumps form when the turbulence shocks;
//   remnant  the freely expanding ejecta of a supernova, a flat core inside a
//            steep power-law envelope (Chevalier 1982), in which the density
//            fluctuates by a few per cent; no bubble, shell or filaments;
//   2, 3     the textbook shock tube and point explosion, for validation.

#include "lib/hydro.slang"

// Random fields with one period across the box: rgb = velocity with a
// Burgers spectrum, unit rms per component; a = density fluctuation, unit rms.
[[vk::binding(1, 0)]] Sampler3D seed_field;
[[vk::binding(2, 0)]] [[vk::image_format("rgba32f")]] RWTexture3D<float4> next_a;
[[vk::binding(3, 0)]] [[vk::image_format("rgba32f")]] RWTexture3D<float4> next_b;

[shader("compute")]
[numthreads(8, 8, 8)]
void csMain(uint3 id : SV_DispatchThreadID) {
    int n = cellsPerEdge();
    if (any(id >= uint3(n, n, n))) {
        return;
    }
    int3 cell = int3(id);
    float3 position = cellCentre(cell);
    float half_extent = hydro.scenario.y;
    int mode = scenario();
    // The random fields repeat over the cloud's size (the nebula) or the box.
    float period = mode == SCENARIO_NEBULA ? 4.0f * hydro.source[1].z : 2.0f * half_extent;
    float4 seed = seed_field.SampleLevel(position / period + 0.5f, 0.0f);

    Pair w;
    w.a = float4(1.0f, 0.0f, 0.0f, 0.0f);
    w.b = float4(1.0f, 0.0f, 0.0f, 0.0f);
    if (mode == SCENARIO_NEBULA) {
        float sound = hydro.scenario.z;
        float cloud = hydro.source[1].x;
        float ambient = hydro.source[1].y;
        float radius = hydro.source[1].z;
        // The cloud's edge is a tenth of its radius wide.
        float inside = 1.0f - smoothstep(0.9f * radius, 1.1f * radius, length(position));
        float density = lerp(ambient, cloud, inside);
        w.a = float4(density, hydro.source[1].w * inside * seed.rgb);
        w.b = float4(density * sound * sound, 0.0f, 0.0f, 0.0f);
    } else if (mode == SCENARIO_REMNANT) {
        float age = hydro.source[1].x;
        float core_velocity = hydro.source[1].y;
        float radius = length(position);
        float velocity = radius / age;
        if (velocity < hydro.source[2].w) {
            // Homologous expansion: a flat core, then a steep envelope.
            float profile = velocity < core_velocity
                                ? 1.0f
                                : pow(velocity / core_velocity, -hydro.source[1].w);
            // The fluctuations belong to the ejecta, so they are laid out in
            // velocity (the coordinate that expansion preserves): one period
            // of the random field spans eight times the ejecta's outer edge
            // (2.5 core velocities), so that its finest modes, a thirtieth of
            // the period, are a few cells wide rather than below the grid.
            float clump = seed_field.SampleLevel(position / (20.0f * age * core_velocity) + 0.5f, 0.0f).a;
            float density = hydro.source[1].z * profile *
                            max(1.0f + hydro.source[2].z * clump, 0.2f);
            w.a = float4(max(density, hydro.source[2].x), position / age);
            w.b = float4(w.a.x * hydro.scenario.z, 1.0f, 0.0f, 0.0f);
            float bubble = hydro.source[3].z;
            if (bubble > 0.0f && radius < bubble) {
                // The pulsar's wind up to now: its energy and (light) mass in
                // a bubble that expands with the ejecta around it.
                float volume = 4.18879f * bubble * bubble * bubble;
                float energy = hydro.source[3].y / volume;
                float wind_speed = max(hydro.source[3].w, 1.0f);
                float wind_density = 2.0f * energy / (wind_speed * wind_speed);
                w.a = float4(max(wind_density, hydro.gas.y), position / age);
                w.b = float4((hydro.gas.x - 1.0f) * energy, 0.0f, 1.0f, 0.0f);
            }
        } else {
            w.a = float4(hydro.source[2].x, 0.0f, 0.0f, 0.0f);
            w.b = float4(hydro.source[2].x * hydro.source[2].y, 0.0f, 0.0f, 0.0f);
        }
    } else if (mode == SCENARIO_SHOCK_TUBE) {
        bool left = position.x < 0.0f;
        w.a = float4(left ? hydro.source[0].x : hydro.source[0].z, 0.0f, 0.0f, 0.0f);
        w.b = float4(left ? hydro.source[0].y : hydro.source[0].w, left ? 1.0f : 0.0f, 0.0f, 0.0f);
    } else {
        // Energy spread evenly over the cells inside the deposit radius.
        float deposit = hydro.source[0].y;
        float volume = 4.18879f * deposit * deposit * deposit;
        bool inside = length(position) < deposit;
        w.a = float4(hydro.source[0].z, 0.0f, 0.0f, 0.0f);
        w.b = float4(hydro.source[0].w +
                         (inside ? (hydro.gas.x - 1.0f) * hydro.source[0].x / volume : 0.0f),
                     inside ? 1.0f : 0.0f, 0.0f, 0.0f);
    }

    Pair u = toConserved(w);
    next_a[id] = u.a;
    next_b[id] = u.b;
}

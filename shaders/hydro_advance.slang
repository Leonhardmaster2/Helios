// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Deep-sky lab gas dynamics: one Runge-Kutta stage of the Euler equations
// (see lib/hydro.slang for the scheme), followed in the second stage by the
// physics that is not gas dynamics:
//
//   nebula   Photo-ionisation.  Gas the star's photons reach is held at
//            10^4 K, everything else at the temperature of a molecular cloud
//            (both by fast radiative heating and cooling, so the temperature
//            is set, not evolved).  The hundredfold pressure difference is
//            what drives the ionised bubble into the cloud.
//   remnant  The pulsar: energy and a little mass are added at the centre at
//            the spin-down rate.  Ejecta below about a million kelvin radiate
//            their heat away within the age of the remnant and are held at a
//            floor temperature; hotter gas, and the pulsar's wind bubble,
//            cool too slowly to matter and are left alone.  The floor is far
//            above the true 10^4 K: at that temperature the swept-up shell
//            would be a thousandth of a cell thick, and it is the shell's
//            break-up that this run is for.

#define HYDRO_RAYS_BINDING 7
#include "lib/hydro_radiation.slang"

// The state to differentiate ...
[[vk::binding(1, 0)]] Texture3D<float4> state_a;
[[vk::binding(2, 0)]] Texture3D<float4> state_b;
// ... and the state at the start of the step (the same in the first stage).
[[vk::binding(3, 0)]] Texture3D<float4> base_a;
[[vk::binding(4, 0)]] Texture3D<float4> base_b;
[[vk::binding(5, 0)]] [[vk::image_format("rgba32f")]] RWTexture3D<float4> next_a;
[[vk::binding(6, 0)]] [[vk::image_format("rgba32f")]] RWTexture3D<float4> next_b;

Pair loadPrimitive(int3 cell) {
    int last = cellsPerEdge() - 1;
    int3 clamped = clamp(cell, int3(0, 0, 0), int3(last, last, last));
    Pair u;
    u.a = state_a.Load(int4(clamped, 0));
    u.b = state_b.Load(int4(clamped, 0));
    return toPrimitive(u);
}

// van Leer's harmonic limiter: zero at extrema, second order elsewhere.
float4 limited(float4 below, float4 above) {
    float4 product = below * above;
    bool4 monotone = product > 0.0f;
    return select(monotone, 2.0f * product / select(monotone, below + above, 1.0f), 0.0f);
}

Pair slope(Pair lower, Pair centre, Pair upper) {
    Pair result;
    result.a = limited(centre.a - lower.a, upper.a - centre.a);
    result.b = limited(centre.b - lower.b, upper.b - centre.b);
    return result;
}

Pair extrapolated(Pair centre, Pair gradient, float side) {
    Pair result;
    result.a = centre.a + (0.5f * side) * gradient.a;
    result.b = centre.b + (0.5f * side) * gradient.b;
    return result;
}

Pair physicalFlux(Pair w, Pair u, int axis) {
    float normal = w.a[1 + axis];
    Pair flux;
    flux.a = u.a * normal;
    flux.a[1 + axis] += w.b.x;
    flux.b = float4((u.b.x + w.b.x) * normal, u.b.yz * normal, 0.0f);
    return flux;
}

// Flux through a face with the primitive states `l` and `r` on its two sides.
// `shocked` selects the HLL flux.
Pair riemann(Pair l, Pair r, int axis, bool shocked) {
    float ul = l.a[1 + axis];
    float ur = r.a[1 + axis];
    float cl = soundSpeed(l);
    float cr = soundSpeed(r);
    // Davis' bounds on the fastest waves.
    float sl = min(ul - cl, ur - cr);
    float sr = max(ul + cl, ur + cr);
    Pair conserved_l = toConserved(l);
    Pair conserved_r = toConserved(r);
    Pair flux_l = physicalFlux(l, conserved_l, axis);
    Pair flux_r = physicalFlux(r, conserved_r, axis);
    if (sl >= 0.0f) {
        return flux_l;
    }
    if (sr <= 0.0f) {
        return flux_r;
    }

    Pair flux;
    // HLLC resolves contact surfaces, which the instabilities studied here
    // live on, but is unstable where a strong shock lies along the grid (the
    // carbuncle, Quirk 1994); in cells flagged as shocked the more diffusive
    // HLL flux is used on every face.
    if (shocked) {
        float inverse = 1.0f / (sr - sl);
        flux.a = (sr * flux_l.a - sl * flux_r.a + sl * sr * (conserved_r.a - conserved_l.a)) * inverse;
        flux.b = (sr * flux_l.b - sl * flux_r.b + sl * sr * (conserved_r.b - conserved_l.b)) * inverse;
        return flux;
    }
    float ml = l.a.x * (sl - ul);
    float mr = r.a.x * (sr - ur);
    float contact = (r.b.x - l.b.x + ul * ml - ur * mr) / (ml - mr);
    bool from_left = contact >= 0.0f;
    Pair w = from_left ? l : r;
    Pair conserved = from_left ? conserved_l : conserved_r;
    Pair base = from_left ? flux_l : flux_r;
    float speed = from_left ? sl : sr;
    float momentum = from_left ? ml : mr;
    float normal = from_left ? ul : ur;

    float factor = momentum / (speed - contact);
    Pair star;
    star.a = float4(factor, factor * w.a.yzw);
    star.a[1 + axis] = factor * contact;
    star.b = float4(factor * (conserved.b.x / w.a.x +
                              (contact - normal) * (contact + w.b.x / momentum)),
                    factor * w.b.yz, 0.0f);
    flux.a = base.a + speed * (star.a - conserved.a);
    flux.b = base.b + speed * (star.b - conserved.b);
    return flux;
}

[shader("compute")]
[numthreads(8, 8, 8)]
void csMain(uint3 id : SV_DispatchThreadID) {
    int n = cellsPerEdge();
    if (any(id >= uint3(n, n, n))) {
        return;
    }
    int3 cell = int3(id);
    float dt = hydro.grid.z;
    float3 size = cellSizeAt(cell);

    Pair centre = loadPrimitive(cell);
    float shock = 0.0f;
    float3 pressure_jump = float3(0.0f, 0.0f, 0.0f);
    float lowest_pressure = 1.0e30f;
    float divergence = 0.0f;
    Pair change;
    change.a = float4(0.0f, 0.0f, 0.0f, 0.0f);
    change.b = float4(0.0f, 0.0f, 0.0f, 0.0f);
    for (int axis = 0; axis < 3; ++axis) {
        int3 step = int3(axis == 0 ? 1 : 0, axis == 1 ? 1 : 0, axis == 2 ? 1 : 0);
        Pair lower2 = loadPrimitive(cell - 2 * step);
        Pair lower1 = loadPrimitive(cell - step);
        Pair upper1 = loadPrimitive(cell + step);
        Pair upper2 = loadPrimitive(cell + 2 * step);
        Pair slope_lower = slope(lower2, lower1, centre);
        Pair slope_centre = slope(lower1, centre, upper1);
        Pair slope_upper = slope(centre, upper1, upper2);
        // A face is shocked if either of its cells was flagged; both cells
        // see the same two flags, so they agree on the flux between them.
        Pair flux_lower = riemann(extrapolated(lower1, slope_lower, 1.0f),
                                  extrapolated(centre, slope_centre, -1.0f), axis,
                                  max(lower1.b.w, centre.b.w) > 0.5f);
        Pair flux_upper = riemann(extrapolated(centre, slope_centre, 1.0f),
                                  extrapolated(upper1, slope_upper, -1.0f), axis,
                                  max(centre.b.w, upper1.b.w) > 0.5f);
        pressure_jump[axis] = upper1.b.x - lower1.b.x;
        lowest_pressure = min(lowest_pressure, min(lower1.b.x, upper1.b.x));
        divergence += upper1.a[1 + axis] - lower1.a[1 + axis];
        float rate = dt / size[axis];
        change.a -= rate * (flux_upper.a - flux_lower.a);
        change.b -= rate * (flux_upper.b - flux_lower.b);
    }

    // This cell is in a strong shock if the pressure more than doubles
    // across it, along the pressure gradient whatever its direction, and
    // the flow converges.
    if (length(pressure_jump) > lowest_pressure && divergence < 0.0f) {
        shock = 1.0f;
    }

    // Stage 0: U1 = U0 + dt L(U0).  Stage 1: U = (U0 + U1 + dt L(U1)) / 2.
    Pair state;
    state.a = state_a.Load(int4(cell, 0)) + change.a;
    state.b = state_b.Load(int4(cell, 0)) + change.b;
    bool final_stage = hydro.gas.w > 0.5f;
    if (final_stage) {
        state.a = 0.5f * (state.a + base_a.Load(int4(cell, 0)));
        state.b = 0.5f * (state.b + base_b.Load(int4(cell, 0)));
    }

    // Floors: no vacuum, no negative pressure.
    float gamma = hydro.gas.x;
    float density = max(state.a.x, hydro.gas.y);
    float3 momentum = state.a.yzw;
    float kinetic = 0.5f * dot(momentum, momentum) / density;
    float thermal = max(state.b.x - kinetic, density * hydro.gas.z / (gamma - 1.0f));
    float2 scalars = clamp(state.b.yz, float2(0.0f, 0.0f), float2(density, density));

    if (final_stage) {
        int mode = scenario();
        float3 position = cellCentre(cell);
        if (mode == SCENARIO_NEBULA) {
            float ionised = ionisedAt(position, density, 1.0f);
            float neutral_sound = hydro.scenario.z;
            float ionised_sound = hydro.scenario.w;
            float sound2 = lerp(neutral_sound * neutral_sound, ionised_sound * ionised_sound, ionised);
            thermal = density * sound2 / (gamma - 1.0f);
            scalars.x = density * ionised;
        } else if (mode == SCENARIO_REMNANT) {
            // The wind enters through a smooth kernel (1 - x^2)^2, so that the
            // mesh does not imprint its axes on it.
            float inject = hydro.source[0].x;
            float x2 = dot(position, position) / (inject * inject);
            if (x2 < 1.0f) {
                float kernel = (1.0f - x2) * (1.0f - x2) / (0.957438f * inject * inject * inject);
                float added = dt * hydro.source[0].z * kernel;
                density += added;
                scalars.y += added; // the wind's own material
                thermal += dt * hydro.source[0].y * kernel;
            }
            // Ejecta that a shock has not heated beyond the peak of the
            // cooling curve return to the floor temperature at once.  Gas
            // that holds even a little of the wind is the contact layer of
            // the bubble, whose heat is the wind's and does not radiate away.
            // The cooled ejecta; in an old remnant (hydro.source[3].x) also
            // the swept-up gas, once its shock has slowed below about
            // 230 km/s, where the post-shock gas cools within the age.
            bool ejecta = scalars.x > 0.5f * density;
            float limit = hydro.scenario.w * (ejecta ? 1.0f : 0.4f);
            bool cools = thermal * (gamma - 1.0f) < density * limit &&
                         scalars.y < 1.0e-3f * density;
            if ((ejecta || hydro.source[3].x > 0.5f) && cools) {
                // The swept-up shell is held twice as warm as cooled ejecta:
                // at the true 10^4 K it would be far thinner than a cell.
                float floor = hydro.scenario.z * (ejecta ? 1.0f : 2.0f);
                thermal = min(thermal, density * floor / (gamma - 1.0f));
            }
        }
    }

    next_a[id] = float4(density, momentum);
    next_b[id] = float4(thermal + 0.5f * dot(momentum, momentum) / density, scalars, shock);
}

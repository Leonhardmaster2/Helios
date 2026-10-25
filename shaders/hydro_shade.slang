// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Deep-sky lab gas dynamics: what the simulated gas looks like.
//
// Converts the simulation state into the volume the display pass marches
// through: rgb = light emitted per unit length (linear sRGB), a = dust.
// Every term is a function of simulated quantities only.
//
//   nebula   Ionised gas recombines and radiates in proportion to n^2.
//            Which lines dominate depends on the ionisation parameter U
//            (ionising photons per atom): [O III] where U is high, near the
//            star and in thin gas; [N II] and [S II] where it is low, at the
//            dense edges.  Dust scatters the star's visible light, dimmed by
//            the dust between the star and the scattering point.
//   remnant  Four kinds of light.  Gas heated by the shocks to millions of
//            kelvin radiates bremsstrahlung, flat across the visible, in
//            proportion to n^2 / sqrt(T): the fingers of shocked ejecta.
//            Ejecta that have cooled again radiate lines in proportion to
//            their density squared, [O III] where thin, low-ionisation lines
//            in dense knots.  The blast wave itself glows in the Balmer lines
//            of the hydrogen atoms it overruns (a "Balmer-dominated" shock):
//            a thin rim.  The pulsar's wind bubble shines by synchrotron
//            radiation; for particles and field in equipartition its
//            emissivity scales as (pressure)^(7/4).
//
// Not modelled: line transfer (the lines are optically thin), the detailed
// ionisation balance behind the zone boundaries, and the composition of the
// ejecta (helium- and nitrogen-rich in a real remnant).

#define HYDRO_RAYS_BINDING 3
#include "lib/hydro_radiation.slang"

[[vk::binding(1, 0)]] Texture3D<float4> state_a;
[[vk::binding(2, 0)]] Texture3D<float4> state_b;
[[vk::binding(4, 0)]] [[vk::image_format("rgba16f")]] RWTexture3D<float4> emission_out;

// hydro.shade:
//   [0] rgb = hydrogen recombination lines per unit n^2
//   [1] rgb = [O III] at high ionisation, w = ionisation parameter (nebula)
//       or ejecta density (remnant) at the zone boundary
//   [2] rgb = [N II] + [S II] at low ionisation
//   [3] nebula:  rgb = scattered starlight per unit density at 1 pc,
//                w = visual optical depth per (cm^-3 pc)
//       remnant: rgb = synchrotron light at the reference pressure,
//                w = 1 / reference pressure
//   [4] x = dust written per unit density of dusty gas,
//       y = largest value written (keeps the half-float volume finite)
//   [5] remnant: rgb = thermal light of hot plasma per (density^2 / sound
//       speed), w = light of the blast wave per unit density, relative to
//       the recombination lines of [0]

// Ionisation parameter per unit (Stromgren integral / (r^2 n)), with r in pc
// and n in cm^-3: U = Q / (4 pi r^2 n c).
static const float IONISATION_PARAMETER = 2.664e-5f;

[shader("compute")]
[numthreads(8, 8, 8)]
void csMain(uint3 id : SV_DispatchThreadID) {
    int n = cellsPerEdge();
    if (any(id >= uint3(n, n, n))) {
        return;
    }
    int3 cell = int3(id);
    Pair u;
    u.a = state_a.Load(int4(cell, 0));
    u.b = state_b.Load(int4(cell, 0));
    Pair w = toPrimitive(u);
    float density = w.a.x;
    float3 position = cellCentre(cell);

    float3 emission = float3(0.0f, 0.0f, 0.0f);
    float dust = 0.0f;
    if (scenario() == SCENARIO_NEBULA) {
        Radiation radiation = radiationAt(position);
        float ionised = ionisedAt(position, density, 1.5f);
        float r2 = radiation.distance * radiation.distance;
        float recombination = density * density * ionised * ionised;
        float parameter = IONISATION_PARAMETER * hydro.source[0].w * max(1.0f - radiation.used, 0.0f) /
                          (r2 * density);
        float high = parameter / (parameter + hydro.shade[1].w);
        emission = recombination *
                   (hydro.shade[0].rgb + high * hydro.shade[1].rgb + (1.0f - high) * hydro.shade[2].rgb);
        // Reflection: starlight reaching this point through the dust.
        float lit = hydro.source[0].w > 0.0f ? exp(-hydro.shade[3].w * radiation.column) : 0.0f;
        emission += hydro.shade[3].rgb * (density * lit / (r2 + 0.01f));
        dust = density;
    } else {
        float wind = w.b.z;
        // Gas that may have cooled and radiates lines: the ejecta, or in an
        // old radiative remnant all the matter that is not the pulsar's wind.
        float ejecta = hydro.source[3].x > 0.5f ? density * (1.0f - wind) : density * w.b.y;
        float pressure = w.b.x * hydro.shade[3].w;
        emission = hydro.shade[3].rgb * (wind * pressure * sqrt(pressure) * sqrt(sqrt(pressure)));

        // p / rho is the temperature; the floor is that of cooled gas.  Gas
        // that was shocked and has cooled sits at the floor; gas that was
        // never shocked (free ejecta, the undisturbed surroundings) is far
        // below it and has nothing to radiate.
        float temperature = w.b.x / density;
        float floor = hydro.scenario.z;
        float cooled = smoothstep(0.3f * floor, 0.7f * floor, temperature) *
                       (1.0f - smoothstep(1.5f * floor, 3.0f * floor, temperature));
        float hot = smoothstep(1.5f * floor, 3.0f * floor, temperature);
        float high = hydro.shade[1].w / (ejecta + hydro.shade[1].w);
        emission += cooled * ejecta * ejecta *
                    (hydro.shade[0].rgb + high * hydro.shade[1].rgb + (1.0f - high) * hydro.shade[2].rgb);
        // Everything hot that is not the wind of the pulsar: shocked ejecta
        // and shocked surrounding gas.
        float matter = density * (1.0f - wind);
        emission += hydro.shade[5].rgb * (hot * matter * matter / sqrt(temperature));
        // The blast wave, where it is crossing the surrounding gas.
        float surrounding = saturate(1.0f - w.b.y - wind);
        emission += hydro.shade[0].rgb * (hydro.shade[5].w * w.b.w * surrounding * density);
        dust = cooled * ejecta;
    }
    float ceiling = hydro.shade[4].y;
    emission_out[id] = float4(min(emission, float3(ceiling, ceiling, ceiling)),
                              min(dust * hydro.shade[4].x, ceiling));
}

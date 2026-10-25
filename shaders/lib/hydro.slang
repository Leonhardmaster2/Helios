// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Compressible gas dynamics on the GPU, shared by the deep-sky lab's
// simulation passes (hydro_init, hydro_advance, hydro_speed, hydro_rays,
// hydro_shade).
//
// The Euler equations for an ideal gas are solved on a uniform grid of N^3
// cells with a finite-volume Godunov scheme: piecewise-linear reconstruction
// of the primitive variables with the van Leer limiter, the HLLC approximate
// Riemann solver (HLL inside strong shocks), and a two-stage Runge-Kutta
// step.  Two passive scalars are advected with the mass.  Boundaries are
// open (zero gradient).
//
// State, two float4 textures per cell:
//   a = (density, momentum x, y, z)
//   b = (total energy, density * scalar 1, density * scalar 2, shock flag)
// The flag marks cells inside a strong shock, where the Riemann solver falls
// back from HLLC to HLL (see hydro_advance.slang).
//
// The grid is stretched: along each axis the index coordinate xi in [-1, 1]
// maps to the position x = L sinh(a xi) / sinh(a), so cells are finest at
// the centre, where the object is, and grow smoothly towards the edges of
// the box (by cosh(a) at the edge: 3.8 for a = 2).  The same cell count then
// covers a box several times wider than the fine region, and what flows out
// of the fine region is still followed, at lower resolution, instead of
// leaving the simulation.  a = 0 is the uniform grid (the validation runs).
// The reconstruction is done in index space, which costs a little accuracy
// where the stretching is strong.
//
// Scenarios (hydro.scenario.x) and their units:
//   0  nebula        pc, km/s, hydrogen atoms per cm^3
//   1  remnant       pc, 1000 km/s (so the time unit is 978 yr), solar masses
//   2  shock tube    Sod's test problem, for validation
//   3  blast         Sedov-Taylor point explosion, for validation
#pragma once

struct HydroParams {
    // x = cells per edge, y = cell size, z = time step, w = time
    float4 grid;
    // x = adiabatic index, y = density floor, z = floor of pressure / density,
    // w = Runge-Kutta stage (0 or 1)
    float4 gas;
    // x = scenario, y = half extent of the box, z and w: see below
    float4 scenario;
    // nebula:  scenario.z = sound speed of neutral gas, .w = of ionised gas
    //   [0] xyz = position of the ionising star, w = Stromgren integral
    //       Q / (4 pi alpha_B) in pc^3 cm^-6 (0 while the star is off)
    //   [1] x = cloud density, y = ambient density, z = cloud radius,
    //       w = rms turbulent speed
    //   [2] x = outer radius of the ray grid
    // remnant: scenario.z = (sound speed)^2 that radiating ejecta cool to,
    //          .w = pressure / density above which ejecta do not cool
    //   [0] x = injection radius, y = energy injected per unit time,
    //       z = mass injected per unit time
    //   [1] x = age at the start, y = core velocity, z = core density at the
    //       start, w = power-law index of the envelope
    //   [2] x = ambient density, y = ambient pressure / density,
    //       z = amplitude of the density fluctuations, w = fastest ejecta
    //   [3] x = 1 when the swept-up gas may cool as well (an old, radiative
    //       remnant); 0 when only the ejecta do; y = energy the pulsar's wind
    //       has put out before the start, z = radius of the bubble holding
    //       it, w = speed the wind's energy gives its mass
    // shock tube: [0] = (left density, left pressure, right density, right pressure)
    // blast:      [0] = (energy, deposit radius, density, pressure)
    float4 source[5];
    // Parameters of the emission model (hydro_shade).
    float4 shade[6];
    // Stretching of the grid: x = a (0: uniform), y = sinh(a), z = the
    // central cell relative to the mean cell, a / sinh(a); w = unused
    float4 mesh;
};

[[vk::binding(0, 0)]] ConstantBuffer<HydroParams> hydro;

static const int SCENARIO_NEBULA = 0;
static const int SCENARIO_REMNANT = 1;
static const int SCENARIO_SHOCK_TUBE = 2;
static const int SCENARIO_BLAST = 3;

// Conserved or primitive state of one cell.
struct Pair {
    float4 a;
    float4 b;
};

int cellsPerEdge() {
    return int(hydro.grid.x);
}

int scenario() {
    return int(hydro.scenario.x + 0.5f);
}

// Index coordinate xi in [-1, 1] -> position in units of the half extent.
float stretchIndex(float xi) {
    float a = hydro.mesh.x;
    return a > 0.0f ? sinh(a * xi) / hydro.mesh.y : xi;
}

float3 stretchIndex(float3 xi) {
    return float3(stretchIndex(xi.x), stretchIndex(xi.y), stretchIndex(xi.z));
}

// Position in units of the half extent -> index coordinate.
float unstretch(float x) {
    float a = hydro.mesh.x;
    if (a <= 0.0f) {
        return x;
    }
    float s = hydro.mesh.y * x;
    return log(s + sqrt(s * s + 1.0f)) / a;
}

float3 unstretch(float3 x) {
    return float3(unstretch(x.x), unstretch(x.y), unstretch(x.z));
}

// Texture coordinate in [0, 1]^3 of a position in the box.
float3 gridCoordinate(float3 position) {
    return unstretch(position / hydro.scenario.y) * 0.5f + 0.5f;
}

// Size of the cell at index coordinate xi, per axis.
float3 cellSize(float3 xi) {
    float a = hydro.mesh.x;
    float3 scale = a > 0.0f ? a * cosh(a * xi) / hydro.mesh.y : float3(1.0f, 1.0f, 1.0f);
    return hydro.grid.y * scale;
}

float3 indexCoordinate(int3 cell) {
    return (float3(cell) + 0.5f) * (2.0f / hydro.grid.x) - 1.0f;
}

float3 cellSizeAt(int3 cell) {
    return cellSize(indexCoordinate(cell));
}

// Volume of a cell relative to the mean cell volume.
float cellVolumeFactor(int3 cell) {
    float3 size = cellSizeAt(cell) / hydro.grid.y;
    return size.x * size.y * size.z;
}

// Mean cell size at a position: how thick a front a cell can hold there.
float localCell(float3 position) {
    float3 size = cellSize(unstretch(position / hydro.scenario.y));
    return (size.x + size.y + size.z) * (1.0f / 3.0f);
}

// Centre of a cell in scene units; the box is [-L, L]^3.
float3 cellCentre(int3 cell) {
    return hydro.scenario.y * stretchIndex(indexCoordinate(cell));
}

// The nebula's ray grid is stretched in radius the same way: w in [0, 1]
// -> radius in units of the outer radius.
float stretchRadius(float w) {
    float a = hydro.mesh.x;
    return a > 0.0f ? sinh(a * w) / hydro.mesh.y : w;
}

// d radius / d w, in units of the outer radius.
float stretchRadiusSlope(float w) {
    float a = hydro.mesh.x;
    return a > 0.0f ? a * cosh(a * w) / hydro.mesh.y : 1.0f;
}

float unstretchRadius(float r) {
    return unstretch(r);
}

// (density, velocity) and (pressure, scalar 1, scalar 2, shock flag).
Pair toPrimitive(Pair u) {
    float density = max(u.a.x, hydro.gas.y);
    float3 velocity = u.a.yzw / density;
    float kinetic = 0.5f * density * dot(velocity, velocity);
    float pressure = max((hydro.gas.x - 1.0f) * (u.b.x - kinetic), density * hydro.gas.z);
    Pair w;
    w.a = float4(density, velocity);
    w.b = float4(pressure, saturate(u.b.yz / density), u.b.w);
    return w;
}

Pair toConserved(Pair w) {
    Pair u;
    u.a = float4(w.a.x, w.a.x * w.a.yzw);
    u.b = float4(w.b.x / (hydro.gas.x - 1.0f) + 0.5f * w.a.x * dot(w.a.yzw, w.a.yzw),
                 w.a.x * w.b.yz, 0.0f);
    return u;
}

float soundSpeed(Pair w) {
    return sqrt(hydro.gas.x * w.b.x / w.a.x);
}

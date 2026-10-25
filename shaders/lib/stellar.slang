// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Single stars of the galaxy, as examples/deep_sky_lab/stellar_systems.cpp
// computes them: the same hash, the same Kroupa (2001) mass function, the
// same main-sequence relations, giant branch and remnants, in float.  Keep
// the two in step; the CPU side is the reference (it picks the star a click
// lands on and builds its planets).
#pragma once

static const float IMF_MINIMUM = 0.08f;
static const float IMF_MAXIMUM = 120.0f;
// Star counts of the two segments of the mass function (unnormalised):
// (0.08^-0.3 - 0.5^-0.3) / 0.3 and 0.5 (0.5^-1.3 - 120^-1.3) / 1.3.
static const float IMF_LOW_COUNT = 3.007530f;
static const float IMF_HIGH_COUNT = 0.946272f;
static const float SOLAR_KELVIN = 5772.0f;
static const float GIANT_SLOPE = 0.75f;

uint pcgHash(uint value) {
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

uint starHash(uint particle, uint rank, uint stream) {
    uint h = pcgHash(particle * 0x9E3779B1u + stream * 0x85EBCA77u);
    return pcgHash(h ^ (rank * 0xC2B2AE3Du + 0x27D4EB2Fu));
}

float starUnit(uint particle, uint rank, uint stream) {
    return (float(starHash(particle, rank, stream) >> 8u) + 0.5f) * (1.0f / 16777216.0f);
}

// Initial mass of the star whose upper tail holds `fraction` of all stars.
float imfTailMass(float fraction) {
    float total = IMF_LOW_COUNT + IMF_HIGH_COUNT;
    float count = clamp(fraction, 0.0f, 1.0f) * total;
    if (count <= IMF_HIGH_COUNT) {
        return clamp(pow(count * (1.3f / 0.5f) + pow(IMF_MAXIMUM, -1.3f), -1.0f / 1.3f), IMF_MINIMUM, IMF_MAXIMUM);
    }
    return clamp(pow((count - IMF_HIGH_COUNT) * 0.3f + pow(0.5f, -0.3f), -1.0f / 0.3f), IMF_MINIMUM, IMF_MAXIMUM);
}

float mainSequenceLuminosity(float mass) {
    if (mass < 0.43f) {
        return 0.23f * pow(mass, 2.3f);
    }
    if (mass < 2.0f) {
        return mass * mass * mass * mass;
    }
    if (mass < 20.0f) {
        return 1.4f * pow(mass, 3.5f);
    }
    return 5.0e4f * pow(mass / 20.0f, 2.2f);
}

float mainSequenceRadius(float mass) {
    return pow(mass, mass < 1.0f ? 0.8f : 0.57f);
}

float mainSequenceMyr(float mass) {
    float x = min(log10(max(mass, IMF_MINIMUM)), 2.0f);
    return pow(10.0f, 4.0f - 3.6f * x + 0.9f * x * x);
}

float giantEnergyRatio(float mass) {
    return clamp(3.5f * pow(mass, -1.2f), 0.15f, 3.5f);
}

float giantTemperature(float mass) {
    if (mass <= 2.0f) {
        return 4400.0f;
    }
    if (mass <= 8.0f) {
        return 4400.0f - 400.0f * log(mass / 2.0f) / log(4.0f);
    }
    if (mass <= 23.0f) {
        return 4000.0f;
    }
    if (mass >= 27.0f) {
        return 30000.0f;
    }
    return 4000.0f * pow(30000.0f / 4000.0f, (mass - 23.0f) / 4.0f);
}

float temperatureOf(float luminosity, float radius) {
    return SOLAR_KELVIN * pow(max(luminosity, 0.0f) / (radius * radius), 0.25f);
}

float radiusOf(float luminosity, float kelvin) {
    float ratio = SOLAR_KELVIN / kelvin;
    return sqrt(max(luminosity, 0.0f)) * ratio * ratio;
}

struct StarLight {
    float radius;  // solar radii
    float kelvin;  // effective temperature; 0 for a black hole
};

// The state of a star of this initial mass at this age (Myr).
StarLight evolveStar(float mass, float age) {
    StarLight star;
    float ms_myr = mainSequenceMyr(mass);
    float ms_luminosity = mainSequenceLuminosity(mass);
    float ms_radius = mainSequenceRadius(mass);
    if (age <= ms_myr) {
        float f = age / ms_myr;
        float luminosity = ms_luminosity * (0.75f + 0.5f * f);
        star.radius = ms_radius * (0.9f + 0.3f * f);
        star.kelvin = temperatureOf(luminosity, star.radius);
        if (star.kelvin > 50000.0f) {
            star.kelvin = 50000.0f;
            star.radius = radiusOf(luminosity, star.kelvin);
        }
        return star;
    }
    // The giant branch: L from the end of the main sequence to the tip, the
    // time per ln L falling as L^-0.75.
    float start = 1.25f * ms_luminosity;
    float tip = mass < 8.0f ? max(start * 1.001f, 2500.0f * clamp(mass / 1.4f, 1.0f, 4.0f)) : start * 1.3f;
    float a = pow(start, -GIANT_SLOPE);
    float b = pow(tip, -GIANT_SLOPE);
    float mean = GIANT_SLOPE / (a - b) * (pow(tip, 1.0f - GIANT_SLOPE) - pow(start, 1.0f - GIANT_SLOPE)) /
                 (1.0f - GIANT_SLOPE);
    float giant_myr = giantEnergyRatio(mass) * ms_luminosity * ms_myr / mean;
    float giant_age = age - ms_myr;
    if (giant_age <= giant_myr) {
        float phase = giant_age / giant_myr;
        float luminosity = pow(a - phase * (a - b), -1.0f / GIANT_SLOPE);
        float end_radius = ms_radius * 1.2f;
        float end_kelvin = min(temperatureOf(start, end_radius), 50000.0f);
        float target = giantTemperature(mass);
        float crossing = 0.3f;
        if (mass < 8.0f) {
            target = clamp(target * pow(luminosity / mean, -0.05f), 2800.0f, 1.3f * target);
            crossing = 0.1f;
        }
        float s = smoothstep(0.0f, crossing, phase);
        star.kelvin = exp(lerp(log(end_kelvin), log(target), s));
        star.radius = radiusOf(luminosity, star.kelvin);
        return star;
    }
    float cooling = max(giant_age - giant_myr, 1.0e-3f);
    if (mass < 8.0f) {
        float dwarf = min(0.109f * mass + 0.394f, 1.35f);
        star.radius = 0.0126f * pow(dwarf / 0.6f, -1.0f / 3.0f);
        float luminosity = clamp(1.0e-3f * (dwarf / 0.6f) * pow(cooling / 1000.0f, -1.4f), 1.0e-6f, 100.0f);
        star.kelvin = temperatureOf(luminosity, star.radius);
    } else if (mass < 20.0f) {
        star.radius = 12.0f / 695700.0f;
        star.kelvin = clamp(1.0e6f * pow(cooling / 0.01f, -0.2f), 1.0e4f, 3.0e6f);
    } else {
        star.radius = 0.0f;
        star.kelvin = 0.0f;
    }
    return star;
}

// Gaussian offset of a star from its particle's centre (pc): the spread in
// the plane, the thickness perpendicular to it.
float3 starOffset(uint particle, uint rank, float spread, float thickness) {
    float u1 = starUnit(particle, rank, 1u);
    float u2 = starUnit(particle, rank, 2u);
    float u3 = starUnit(particle, rank, 3u);
    float u4 = starUnit(particle, rank, 4u);
    float radial = sqrt(-2.0f * log(u1));
    float angle = 6.2831853f * u2;
    return float3(spread * radial * cos(angle), spread * radial * sin(angle),
                  thickness * sqrt(-2.0f * log(u3)) * cos(6.2831853f * u4));
}

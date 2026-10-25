// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Deep-sky lab: a magnetised, rotating neutron star in one of its states
// (cooling in isolation, a rotation-powered pulsar, a magnetar, or accreting
// from a disk).  Everything drawn follows from the dipole field, the spin and
// a few physical parameters; nothing is placed by hand.
//
// Light paths.  Outside a slowly rotating neutron star space-time is, to
// first order, Schwarzschild, so every camera ray is an exact null geodesic
// of that metric, integrated in its orbital plane exactly as in the
// black-hole lab (Binet equation u'' = -u + 3u^2, u = M/r).  A star of
// 1.4 solar masses and 12 km radius has R = 5.8 M: light bends enough that
// well over half of the surface is visible at once, and whatever is behind
// the star is lifted into view around its limb.
//
// Surface.  A black body whose observed temperature is g * T with
//     g = sqrt(1 - 2M/R - Omega^2 R^2 sin^2(theta)) / [lapse_camera (1 - Omega L_z/E)],
// the exact frequency ratio for rigid rotation seen by a static camera: it
// holds the gravitational redshift and the Doppler boost of the approaching
// limb.  T itself is not uniform: heat leaves the core through a crust whose
// conductivity is far higher along the field than across it, so the surface
// is hottest where the field is radial (the magnetic poles) and coolest at
// the magnetic equator (Greenstein & Hartke 1983),
//     T^4 = T_pole^4 (cos^2 Theta_B + chi sin^2 Theta_B),
// Theta_B the angle between the field and the normal, chi the conductivity
// ratio.  Two further heat sources are footprints of the magnetosphere: the
// polar caps where the return current of the open field lines lands, and the
// spots where accreted matter lands, which have the shape of the field lines
// that feed them.
//
// Magnetosphere.  A centred dipole inclined to the spin axis co-rotates with
// the star.  Field lines that cross the light cylinder R_lc = c / Omega are
// open (their equatorial reach L = r / sin^2 theta exceeds R_lc); plasma
// streams out along them with the Goldreich-Julian density ~ |Omega . B| and
// radiates along its motion, so its emissivity towards the camera carries the
// Doppler factor D^(2 + alpha) of a relativistic flow: a beam is bright only
// while it points at the observer, which is why a pulsar pulses.  Beyond the
// light cylinder the field is wound into a spiral and the two magnetic
// hemispheres are separated by a current sheet, whose shape is that of the
// split monopole (Bogovalov 1999),
//     cos(theta) cos(chi) + sin(theta) sin(chi) cos(phi - Omega t + (r - R_lc) / R_lc) = 0;
// it is where the wind dissipates, and is drawn as a thin emitting surface.
// In a magnetar the field is twisted: currents flow along the closed field
// lines near the magnetic axis (those reaching beyond a few stellar radii,
// Beloborodov 2009), their plasma glows and heats the footprints.
//
// Accretion.  A thin Keplerian disk in the spin equator, truncated where the
// magnetic pressure of the dipole balances the ram pressure of the inflow
// (the magnetospheric radius r_m).  From its inner edge matter is loaded onto
// the field lines that cross the disk plane there and falls along them to
// the nearer pole; density follows from mass conservation along the flux
// tube, rho ~ B / v, with free fall from the disk.  The streams are drawn
// optically thin with emissivity ~ rho^2 (thermal plasma); where they land
// they heat the surface to the temperature that radiates the accretion
// luminosity.  The disk is a multi-temperature black body: the Shakura-
// Sunyaev profile of viscous heating plus the heating by the X-rays of the
// inner parts, which rules beyond a few hundred M and keeps the disk at
// 10^4 K out to its tidal edge; Doppler- and gravitationally shifted like
// the black-hole lab's.  Light paths are integrated out to a few hundred M;
// the disk's outer parts are crossed on the straight continuation.
//
// Units: M = GM/c^2 and M/c.  frame.object:
//   [0] x = stellar radius, y = spin Omega, z = magnetic inclination (rad),
//       w = rotation phase Omega * t (rad)
//   [1] x = polar surface temperature (K), y = polar cap temperature (K),
//       z = surface grid strength, w = conductivity ratio chi
//   [2] x = outflow emissivity at the polar cap, in surface radiances per M,
//       y = its Lorentz factor, z = light-cylinder radius, w = equatorial
//       reach L above which field lines are heated at their feet (the cap)
//   [3] x = twist-bundle emissivity, y = its inner reach L_j, z = current-sheet
//       emissivity, w = Lorentz factor of the wind
//   [4] x = magnetospheric radius r_m (0: no disk), y = disk outer radius,
//       z = loaded fraction of r_m (stream width), w = stream emissivity
//   [5] x = disk temperature scale T0 (T^4 = T0^4 (M/r)^3 (1 - sqrt(r_m/r))),
//       y = hot-spot temperature (K), z = band factor (observed temperatures
//       are multiplied by it: 1 for visible light), w = outer radius of the
//       current sheet
//   [6] x = radius beyond which nothing emits, y = speed of the stream where
//       it leaves the disk (c), z = spot heating is on (0/1), w = T_irr^4 r^2
//       of the irradiated disk (K^4 M^2)
//
// Not modelled: frame dragging and oblateness, the distortion of the dipole
// near the light cylinder, light travel-time delays, radiative transfer in
// the streams and the sheet, and the wind's acceleration.  Free parameters
// are stated in the UI: the brightness of each non-thermal component relative
// to the surface (no theory fixes their radiative efficiency), the Lorentz
// factors (real ones are higher, for which beams are visible only when they
// point at the camera), and the stream width.

#include "lib/deep_sky.slang"

static const int MAX_STEPS = 640;
// The non-thermal emission has spectral index alpha = 1/2 (F_nu ~ nu^-alpha).
// A flat non-thermal spectrum is close to the colour of a 12 000 K star; hot
// plasma radiating bremsstrahlung in the visible, of a 10 000 K one.
static const float NONTHERMAL_KELVIN = 12000.0f;
static const float PLASMA_KELVIN = 10000.0f;
static const float SHEET_WIDTH = 0.05f; // of the sheet's function value

// Rows of the rotation from the frame into the star's co-rotating magnetic
// frame, whose z axis is the magnetic axis: undo the rotation about the spin
// axis (frame z), then tilt by the magnetic inclination about the rotating
// x axis.  The same for every sample of a pixel, so it is set up once.
static float3 magnetic_row0;
static float3 magnetic_row1;
static float3 magnetic_row2;
static float3 spin_in_magnetic; // the spin axis in the magnetic frame
static float sheet_sin_inclination;
static float sheet_cos_inclination;

void setMagneticFrame() {
    float s = sin(frame.object[0].w);
    float c = cos(frame.object[0].w);
    float si = sin(frame.object[0].z);
    float ci = cos(frame.object[0].z);
    magnetic_row0 = float3(c, s, 0.0f);
    magnetic_row1 = float3(-s * ci, c * ci, -si);
    magnetic_row2 = float3(-s * si, c * si, ci);
    spin_in_magnetic = float3(magnetic_row0.z, magnetic_row1.z, magnetic_row2.z);
    sheet_sin_inclination = si;
    sheet_cos_inclination = ci;
}

float3 toMagneticFrame(float3 v) {
    return float3(dot(magnetic_row0, v), dot(magnetic_row1, v), dot(magnetic_row2, v));
}

// Fraction of a field line's footprint that lies beyond equatorial reach
// `limit`: sin^2(theta) < r / limit, with an edge a tenth of the bundle wide.
float reachFraction(float sin2_colatitude, float radius, float limit) {
    float boundary = saturate(radius / limit);
    return 1.0f - smoothstep(0.81f * boundary, 1.21f * boundary, sin2_colatitude);
}

// Doppler factor of a flow with Lorentz factor `lorentz` along unit `flow`
// for light travelling along `towards_camera`.
float dopplerFactor(float3 flow, float lorentz, float3 towards_camera) {
    float speed = sqrt(max(1.0f - 1.0f / (lorentz * lorentz), 0.0f));
    return 1.0f / (lorentz * max(1.0f - speed * dot(flow, towards_camera), 1.0e-3f));
}

// D^(2 + alpha) with alpha = 1/2.
float beaming(float doppler) {
    return doppler * doppler * sqrt(doppler);
}

// Where the field line of magnetic azimuth `phi` crosses the disk plane (the
// spin equator): its magnetic colatitude, in (0, pi).
float diskCrossingColatitude(float cos_phi, float sin_phi) {
    float3 s = spin_in_magnetic;
    float transverse = s.x * cos_phi + s.y * sin_phi;
    // sin(theta) transverse + cos(theta) s.z = 0
    return atan2(-s.z, transverse);
}

// ---------------------------------------------------------------------------
// Optically thin emission along the ray
// ---------------------------------------------------------------------------

struct Plasma {
    float3 radiance;    // per unit length
};

// Emissivity of the magnetosphere at `position` (frame coordinates, radius
// `radius`) for light travelling along `towards_camera`.
float3 magnetosphereEmission(float3 position, float radius, float3 towards_camera) {
    float star_radius = frame.object[0].x;
    float light_cylinder = frame.object[2].z;
    float3 total = float3(0.0f, 0.0f, 0.0f);

    float3 m = toMagneticFrame(position) / radius;
    float cos_colatitude = m.z;
    float sin2_colatitude = max(1.0f - cos_colatitude * cos_colatitude, 0.0f);
    float sin_colatitude = sqrt(sin2_colatitude);

    // Dipole field direction: 2 cos(theta) r_hat + sin(theta) theta_hat, and
    // its strength relative to the pole of the surface.
    float3 r_hat = m;
    float3 theta_hat = sin_colatitude > 1.0e-4f
                           ? (cos_colatitude * r_hat - float3(0.0f, 0.0f, 1.0f)) / sin_colatitude
                           : float3(1.0f, 0.0f, 0.0f);
    float3 field = 2.0f * cos_colatitude * r_hat + sin_colatitude * theta_hat;
    float field_length = length(field);
    float3 field_direction = field / field_length;
    float dilution = star_radius / radius;
    float dilution3 = dilution * dilution * dilution;
    float3 light = toMagneticFrame(towards_camera);

    // Outflow on the open field lines, inside the light cylinder.
    float outflow_scale = frame.object[2].x;
    if (outflow_scale > 0.0f && radius < light_cylinder) {
        float open = reachFraction(sin2_colatitude, radius, light_cylinder);
        if (open > 0.0f) {
            // Goldreich-Julian density |Omega . B|, relative to an aligned
            // polar cap (|B| = 2 there in these units).
            float density = abs(dot(spin_in_magnetic, field)) * 0.5f * dilution3;
            // The plasma leaves the star along the field.
            float3 flow = dot(field_direction, r_hat) >= 0.0f ? field_direction : -field_direction;
            float doppler = dopplerFactor(flow, max(frame.object[2].y, 1.0f), light);
            // Fade out where the dipole picture ends.
            float inside = 1.0f - smoothstep(0.8f, 1.0f, radius / light_cylinder);
            total += blackbodyChroma(NONTHERMAL_KELVIN) *
                     (outflow_scale * open * inside * density * beaming(doppler));
        }
    }

    // A magnetar's twisted bundle: currents on the closed field lines whose
    // equatorial reach exceeds L_j.  The plasma near the star is slowed by
    // radiation drag, so it radiates nearly isotropically, by scattering the
    // surface's photons: its emissivity follows the current density, j ~ B,
    // times the photon density, r^-2.
    float twist_scale = frame.object[3].x;
    if (twist_scale > 0.0f) {
        float reach = frame.object[3].y;
        float bundle = reachFraction(sin2_colatitude, radius, reach) *
                       (1.0f - reachFraction(sin2_colatitude, radius, light_cylinder));
        if (bundle > 0.0f) {
            total += blackbodyChroma(NONTHERMAL_KELVIN) *
                     (twist_scale * bundle * field_length * 0.5f * dilution3 * dilution * dilution);
        }
    }

    // Accretion streams: matter on the field lines loaded at the disk's inner
    // edge, between the disk plane and the nearer pole.
    float magnetospheric = frame.object[4].x;
    float stream_scale = frame.object[4].w;
    if (stream_scale > 0.0f && magnetospheric > 0.0f && radius > star_radius &&
        radius < magnetospheric * (1.0f + frame.object[4].z) && sin_colatitude > 1.0e-3f) {
        float cos_phi = m.x / sin_colatitude;
        float sin_phi = m.y / sin_colatitude;
        float crossing = diskCrossingColatitude(cos_phi, sin_phi);
        float sin_crossing = sin(crossing);
        float loaded = magnetospheric / max(sin_crossing * sin_crossing, 1.0e-3f);
        float line_reach = radius / sin2_colatitude;
        float relative = line_reach / loaded - 1.0f;
        float width = frame.object[4].z;
        // Gaussian across the loaded shell, so the stream has soft edges.
        float across = relative / (0.5f * width) - 1.0f;
        float shell = exp(-0.5f * across * across);
        // Only the branch from the crossing towards the nearer pole carries
        // gas: the other way is uphill against gravity.
        float colatitude = acos(clamp(cos_colatitude, -1.0f, 1.0f));
        bool north = crossing < 0.5f * PI;
        float along = north ? crossing - colatitude : colatitude - crossing;
        float branch = smoothstep(-0.03f, 0.03f, along);
        if (shell * branch > 1.0e-3f) {
            // Free fall along the line from the loading radius; v0 where it
            // leaves the disk.
            float v0 = frame.object[6].y;
            float speed = sqrt(max(v0 * v0 + 2.0f * (1.0f / radius - 1.0f / magnetospheric), v0 * v0));
            // rho ~ B / v along the tube, normalised to the loading point.
            float field_at_loading = sqrt(1.0f + 3.0f * cos(crossing) * cos(crossing)) *
                                     pow(star_radius / magnetospheric, 3.0f);
            float density = (field_length * dilution3 / field_at_loading) * (v0 / speed);
            // The gas falls along the field towards the star.
            float3 flow = dot(field_direction, r_hat) <= 0.0f ? field_direction : -field_direction;
            float lorentz = 1.0f / sqrt(max(1.0f - speed * speed, 1.0e-4f));
            float doppler = dopplerFactor(flow, lorentz, light);
            total += blackbodyChroma(PLASMA_KELVIN) *
                     (stream_scale * shell * branch * density * density * beaming(doppler));
        }
    }
    return total;
}

// The wind's current sheet beyond the light cylinder: the split-monopole
// surface
//     cos(theta) cos(chi) + sin(theta) sin(chi) cos(phi - Omega t + (r - R_lc) / R_lc) = 0
// as a signed function of position whose zero set is the sheet.  For an
// aligned rotator it is cos(theta) and the sheet is the equator.  Written
// without the azimuth: cos(phi + a) = (x cos a - y sin a) / (r sin theta).
// Its gradient is bounded by 2 / r + 1 / R_lc, so |value| over that bound is
// a distance the ray can step without crossing the sheet.
float sheetValue(float3 position, float radius) {
    float light_cylinder = frame.object[2].z;
    float a = (radius - light_cylinder) / light_cylinder - frame.object[0].w;
    float s = sin(a);
    float c = cos(a);
    return (position.z * sheet_cos_inclination +
            sheet_sin_inclination * (position.x * c - position.y * s)) / radius;
}

// Abramowitz & Stegun 7.1.26 (error under 2e-7).
float erfApprox(float x) {
    float sign_x = x < 0.0f ? -1.0f : 1.0f;
    x = abs(x);
    float t = 1.0f / (1.0f + 0.3275911f * x);
    float y = 1.0f - (((((1.061405429f * t - 1.453152027f) * t) + 1.421413741f) * t -
                       0.284496736f) * t + 0.254829592f) * t * exp(-x * x);
    return sign_x * y;
}

// Mean of the sheet's profile exp(-v^2 / (2 w^2)) over a segment along
// which the function value runs linearly from v0 to v1: the thin sheet is
// integrated exactly however coarse the step that crosses it.
float sheetProfile(float v0, float v1) {
    float dv = v1 - v0;
    if (abs(dv) < 1.0e-3f * SHEET_WIDTH) {
        float v = 0.5f * (v0 + v1);
        return exp(-0.5f * v * v / (SHEET_WIDTH * SHEET_WIDTH));
    }
    float k = 0.70710678f / SHEET_WIDTH;
    return SHEET_WIDTH * 1.2533141f * (erfApprox(v1 * k) - erfApprox(v0 * k)) / dv;
}

// Emission of the sheet along a segment whose middle is `position`, with
// the sheet function running from `v0` to `v1` along it.
float3 sheetEmission(float3 position, float radius, float3 towards_camera, float v0, float v1) {
    float scale = frame.object[3].z;
    float light_cylinder = frame.object[2].z;
    if (scale <= 0.0f || radius < light_cylinder) {
        return float3(0.0f, 0.0f, 0.0f);
    }
    float sheet = sheetProfile(v0, v1);
    if (sheet < 1.0e-4f) {
        return float3(0.0f, 0.0f, 0.0f);
    }
    // The wind moves radially at its Lorentz factor.  Synchrotron emission
    // per volume goes as the particle density (r^-2) times the field energy
    // (r^-2); the sheet's thickness grows with r, so per unit path it falls
    // as r^-3.
    float doppler = dopplerFactor(position / radius, max(frame.object[3].w, 1.0f), towards_camera);
    float dilution = light_cylinder / radius;
    dilution *= dilution * dilution;
    // Fade in across the first fifth of the light cylinder, where the dipole
    // opens into the wind, and out at the sheet's drawn extent.
    float onset = smoothstep(1.0f, 1.2f, radius / light_cylinder);
    float extent = 1.0f - smoothstep(0.8f, 1.0f, radius / frame.object[5].w);
    return blackbodyChroma(NONTHERMAL_KELVIN) *
           (scale * sheet * onset * extent * dilution * beaming(doppler));
}

// ---------------------------------------------------------------------------
// Surfaces
// ---------------------------------------------------------------------------

// Fraction of the surface point at magnetic direction `m` (unit) that is
// heated by landing accretion streams: the feet of the loaded field lines on
// the branch that carries gas.
float accretionSpot(float3 m, float sin2_colatitude) {
    float magnetospheric = frame.object[4].x;
    if (frame.object[6].z < 0.5f || magnetospheric <= 0.0f || sin2_colatitude < 1.0e-6f) {
        return 0.0f;
    }
    float sin_colatitude = sqrt(sin2_colatitude);
    float crossing = diskCrossingColatitude(m.x / sin_colatitude, m.y / sin_colatitude);
    float sin_crossing = sin(crossing);
    float loaded = magnetospheric / max(sin_crossing * sin_crossing, 1.0e-3f);
    float line_reach = frame.object[0].x / sin2_colatitude;
    float relative = line_reach / loaded - 1.0f;
    float width = frame.object[4].z;
    float across = relative / (0.5f * width) - 1.0f;
    float shell = exp(-0.5f * across * across);
    float colatitude = acos(clamp(m.z, -1.0f, 1.0f));
    bool north = crossing < 0.5f * PI;
    float along = north ? crossing - colatitude : colatitude - crossing;
    return shell * smoothstep(-0.03f, 0.03f, along);
}

// Surface radiance at a point given by its unit direction from the centre.
float3 surface(float3 normal, float impact, float axial, float lapse) {
    float star_radius = frame.object[0].x;
    float omega = frame.object[0].y;
    float3 m = toMagneticFrame(normal);
    float cos_colatitude = m.z;
    float cos2 = cos_colatitude * cos_colatitude;
    float sin2 = max(1.0f - cos2, 0.0f);

    // Anisotropic conduction through the crust: cos^2(Theta_B) of a dipole
    // is 4 cos^2(theta) / (1 + 3 cos^2(theta)).
    float chi = frame.object[1].w;
    float along = 4.0f * cos2 / (1.0f + 3.0f * cos2);
    float temperature = frame.object[1].x * sqrt(sqrt(along + chi * (1.0f - along)));

    // Heated footprints: the polar cap of the field lines reaching beyond the
    // cap limit, and the accretion spots.  Hotter wins.
    float cap = reachFraction(sin2, star_radius, frame.object[2].w);
    temperature = lerp(temperature, max(temperature, frame.object[1].y), cap);
    float spot = accretionSpot(m, sin2);
    temperature = lerp(temperature, max(temperature, frame.object[5].y), spot);

    // Rigid rotation about the frame's z axis.
    float cylindrical2 = star_radius * star_radius * max(1.0f - normal.z * normal.z, 0.0f);
    float time_dilation = sqrt(max(1.0f - 2.0f / star_radius - omega * omega * cylindrical2, 1.0e-4f));
    float shift = time_dilation / (lapse * max(1.0f - omega * axial, 0.05f));

    float band = frame.object[5].z;
    float4 entry = blackbodyEntry(shift * temperature * band);
    float3 radiance = entry.rgb * exp(entry.a - frame.radiometry.x);

    // Limb darkening, with the emission angle of a static surface element:
    // sin(angle) = b sqrt(1 - 2M/R) / R.
    float sine = saturate(impact * sqrt(1.0f - 2.0f / star_radius) / star_radius);
    float mu = sqrt(1.0f - sine * sine);
    radiance *= (1.0f + 2.06f * mu) * (1.0f / 2.3733333f);

    // Optional graticule fixed to the rotating surface (a diagnostic of the
    // light bending; not a physical feature).
    float grid = frame.object[1].z;
    if (grid > 0.0f) {
        float phase = frame.object[0].w;
        float longitude = atan2(normal.y, normal.x) - phase;
        float latitude = asin(clamp(normal.z, -1.0f, 1.0f));
        float2 cell = float2(longitude, latitude) * (12.0f / PI);
        float2 line_distance = abs(frac(cell + 0.5f) - 0.5f);
        line_distance.x *= max(cos(latitude), 0.05f);
        float line_mask = 1.0f - smoothstep(0.02f, 0.05f, min(line_distance.x, line_distance.y));
        radiance *= 1.0f - 0.6f * grid * line_mask;
    }
    return radiance;
}

// Radiance of the disk at radius `radius` in the spin equator, seen by this
// ray (impact parameter, axial angular momentum, plane tilt, camera lapse).
float3 diskRadiance(float radius, float impact, float axial, float tilt, float lapse) {
    float magnetospheric = frame.object[4].x;
    float u = 1.0f / radius;
    float omega = u * sqrt(u);
    // g = sqrt(1 - 3M/r) / (lapse_camera * (1 - Omega * lambda)) for
    // circular geodesic gas and a static camera.
    float gravity = sqrt(max(1.0f - 3.0f * u, 0.0f)) / lapse;
    float doppler = 1.0f / max(1.0f - omega * axial, 0.05f);
    float shift = gravity * doppler;
    // Viscous heating (Shakura-Sunyaev) plus irradiation by the inner parts.
    float scale = frame.object[5].x;
    float scale2 = scale * scale;
    float viscous = scale2 * scale2 * u * u * u * max(1.0f - sqrt(magnetospheric * u), 0.0f);
    float irradiated = frame.object[6].w * u * u;
    float temperature = sqrt(sqrt(viscous + irradiated));
    float band = frame.object[5].z;
    float4 entry = blackbodyEntry(shift * temperature * band);
    float3 radiance = entry.rgb * exp(entry.a - frame.radiometry.x);
    // Limb darkening: cos(emission angle) in the gas frame.
    float mu = saturate(shift * lapse * impact * tilt * u);
    radiance *= (1.0f + 2.06f * mu) * (1.0f / 2.3733333f);
    return radiance;
}

[shader("fragment")]
float4 fsMain(VsOut input) : SV_Target {
    setMagneticFrame();
    float2 pixel = input.pos.xy;
    float3 direction = cameraRay(pixel);
    float3 camera = frame.camera_position.xyz;
    float star_radius = frame.object[0].x;
    float light_cylinder = frame.object[2].z;
    float magnetospheric = frame.object[4].x;
    float disk_outer = frame.object[4].y;
    float stream_outer = magnetospheric * (1.0f + frame.object[4].z) * 1.1f;
    bool has_streams = frame.object[4].w > 0.0f && magnetospheric > 0.0f;
    bool has_sheet = frame.object[3].z > 0.0f;
    bool has_glow = frame.object[2].x > 0.0f || frame.object[3].x > 0.0f || has_streams;
    float glow_outer = has_streams ? max(light_cylinder, stream_outer) : light_cylinder;
    float emission_outer = frame.object[6].x;

    float step_scale = clamp(frame.quality.z, 0.25f, 4.0f);

    float camera_radius = length(camera);
    float3 radial = camera / camera_radius;
    float u0 = 1.0f / camera_radius;
    float lapse = sqrt(1.0f - 2.0f * u0);
    float cos_alpha = dot(direction, radial);
    float3 tangential = direction - cos_alpha * radial;
    float sin_alpha = max(length(tangential), 1.0e-6f);
    float3 tangent = tangential / sin_alpha;
    float3 plane_normal = cross(radial, tangent);
    float impact = sin_alpha / (u0 * lapse);
    float axial = -impact * plane_normal.z;
    float tilt = sqrt(saturate(1.0f - plane_normal.z * plane_normal.z));

    float u = u0;
    float v = -u0 * lapse * cos_alpha / sin_alpha;
    float phi = 0.0f;
    float surface_u = 1.0f / star_radius;

    float3 color = float3(0.0f, 0.0f, 0.0f);
    float transmittance = 1.0f; // through the disk's soft edges
    float3 sky_direction = direction;
    bool escaped = false;
    bool hit = false;
    float jitter = marchJitter(pixel);
    float3 previous = camera;
    float sheet_outer = frame.object[5].w;
    float sheet_value = has_sheet ? sheetValue(camera, camera_radius) : 0.0f;

    // Far enough out that the rest of the path is straight to well below a
    // pixel, and past everything that emits.
    float escape_u = 1.0f / max(4.0f * camera_radius, emission_outer);
    for (int step = 0; step < MAX_STEPS; ++step) {
        if (v < 0.0f && u < escape_u) {
            // Receding and far away: follow the tangent to infinity.
            float escape_angle = phi + u / -v;
            sky_direction = cos(escape_angle) * radial + sin(escape_angle) * tangent;
            escaped = true;
            break;
        }
        // Steps of bounded arc length: fine through the streams and the
        // sheet, moderate inside the light cylinder, long where the ray only
        // needs to be bent.
        float radius = 1.0f / u;
        float arc;
        if (has_streams && radius < stream_outer) {
            arc = max(0.15f, 0.05f * radius);
        } else if (radius < light_cylinder) {
            arc = max(0.3f, 0.1f * radius);
        } else if (has_sheet && radius < sheet_outer) {
            // Steps bounded by the distance to the sheet, and fine enough
            // across it for the factors that change with radius (the sheet
            // itself is integrated analytically over each step).
            float fine = 0.1f * min(radius, 2.0f * light_cylinder);
            arc = max(fine, 0.8f * abs(sheet_value) / (2.0f / radius + 1.0f / light_cylinder));
        } else {
            arc = 0.14f * radius;
        }
        arc *= step_scale;
        if (step == 0) {
            arc *= 0.25f + 0.75f * jitter;
        }
        float h = min(arc * u * u / sqrt(u * u + v * v), 0.2f);

        float k1u = v;
        float k1v = u * (3.0f * u - 1.0f);
        float ua = u + 0.5f * h * k1u;
        float k2u = v + 0.5f * h * k1v;
        float k2v = ua * (3.0f * ua - 1.0f);
        float ub = u + 0.5f * h * k2u;
        float k3u = v + 0.5f * h * k2v;
        float k3v = ub * (3.0f * ub - 1.0f);
        float uc = u + h * k3u;
        float k4u = v + h * k3v;
        float k4v = uc * (3.0f * uc - 1.0f);
        float next_u = u + h * (k1u + 2.0f * (k2u + k3u) + k4u) * (1.0f / 6.0f);
        float next_v = v + h * (k1v + 2.0f * (k2v + k3v) + k4v) * (1.0f / 6.0f);

        if (next_u <= 0.0f) {
            float s = u / (u - next_u);
            float escape_angle = phi + s * h;
            sky_direction = cos(escape_angle) * radial + sin(escape_angle) * tangent;
            escaped = true;
            break;
        }

        float fraction = 1.0f;
        if (next_u >= surface_u) {
            // Landed inside the star: stop on the surface.
            fraction = saturate((surface_u - u) / (next_u - u));
            hit = true;
        }
        float end_angle = phi + fraction * h;
        float end_u = lerp(u, next_u, fraction);
        float3 position = (cos(end_angle) * radial + sin(end_angle) * tangent) / end_u;
        float end_radius = 1.0f / end_u;
        bool glowing = (has_glow && min(radius, end_radius) < glow_outer) ||
                       (has_sheet && end_radius > light_cylinder);

        // The disk: an opaque surface in the spin equator between r_m and
        // its outer radius, with soft edges.  Crossings are found from the
        // sign of z; the glow up to the crossing is added first.
        if (magnetospheric > 0.0f && previous.z * position.z < 0.0f) {
            float s = previous.z / (previous.z - position.z);
            float3 crossing = lerp(previous, position, s);
            float crossing_radius = length(crossing);
            float alpha = smoothstep(magnetospheric, magnetospheric * 1.04f, crossing_radius) *
                          (1.0f - smoothstep(0.9f * disk_outer, disk_outer, crossing_radius));
            if (alpha > 0.0f) {
                if (glowing) {
                    float3 segment = previous - crossing;
                    float segment_length = length(segment);
                    if (segment_length > 0.0f) {
                        float3 middle = 0.5f * (previous + crossing);
                        float middle_radius = length(middle);
                        float3 to_camera = segment / segment_length;
                        color += transmittance * segment_length *
                                 (magnetosphereEmission(middle, middle_radius, to_camera) +
                                  sheetEmission(middle, middle_radius, to_camera, sheet_value,
                                                sheet_value));
                    }
                }
                color += transmittance * alpha *
                         diskRadiance(crossing_radius, impact, axial, tilt, lapse);
                transmittance *= 1.0f - alpha;
                if (transmittance < 0.002f) {
                    hit = true;
                    break;
                }
                previous = crossing;
            }
        }

        // Optically thin plasma along the bent path.
        float sheet_next = has_sheet ? sheetValue(position, end_radius) : 0.0f;
        if (glowing) {
            float3 segment = previous - position; // the way the light travels
            float segment_length = length(segment);
            float3 middle = 0.5f * (previous + position);
            float middle_radius = length(middle);
            if (middle_radius > star_radius && segment_length > 0.0f) {
                float3 to_camera = segment / segment_length;
                if (has_glow && middle_radius < glow_outer) {
                    color += transmittance * segment_length *
                             magnetosphereEmission(middle, middle_radius, to_camera);
                }
                if (has_sheet && middle_radius > light_cylinder) {
                    color += transmittance * segment_length *
                             sheetEmission(middle, middle_radius, to_camera, sheet_value, sheet_next);
                }
            }
        }
        previous = position;
        sheet_value = sheet_next;

        if (hit) {
            color += transmittance * surface(position * end_u, impact, axial, lapse);
            break;
        }
        u = next_u;
        v = next_v;
        phi += h;
    }

    // Beyond the integrated path the ray is straight (the deflection left
    // is 4M/r, under a part in a thousand); the disk's outer parts, out to
    // the tidal edge, are crossed there.
    if (escaped && magnetospheric > 0.0f && transmittance > 0.002f &&
        previous.z * sky_direction.z < 0.0f) {
        float3 crossing = previous - sky_direction * (previous.z / sky_direction.z);
        float crossing_radius = length(crossing);
        float alpha = smoothstep(magnetospheric, magnetospheric * 1.04f, crossing_radius) *
                      (1.0f - smoothstep(0.9f * disk_outer, disk_outer, crossing_radius));
        if (alpha > 0.0f) {
            color += transmittance * alpha * diskRadiance(crossing_radius, impact, axial, tilt, lapse);
            transmittance *= 1.0f - alpha;
        }
    }

    // Screen-space derivatives of the lensed direction: the sky footprint,
    // with the magnification and shear of the lensing.
    SkyFootprint footprint;
    footprint.dx = ddx(sky_direction);
    footprint.dy = ddy(sky_direction);
    if (escaped) {
        color += transmittance * skyRadiance(blackbody_table, frame.radiometry, frame.sky,
                                             sky_direction, footprint, lapse);
    }
    return float4(color, 1.0f);
}

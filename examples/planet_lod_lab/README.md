# Helios Planet LOD / Descent Lab

This is the interactive Vulkan proof for Helios v0.0.1. It creates a Vulkan
device and swap chain through Diligent Engine, renders instanced procedural
cube-sphere patches, and exposes the complete inspection policy through a
Diligent ImGui overlay.

The scripted route begins at roughly one billion metres body-centre distance,
passes through 100 million and 20 million metres, then visits 1000 km, 100 km,
10 km, 1 km, 100 m, and 10 m before reaching a two-metre eye height above the
procedural terrain. At every scale the CPU quadtree selects tiles by
screen-space error, streams camera-relative double-to-float patch anchors, and
draws them with an infinite reversed-Z projection. Four small skirts on every
patch hide ordinary quadtree T-junctions; in the natural view they shade like
the surface, and the wire and debug-colour views darken them so they can be
inspected. The UI exposes split/merge thresholds,
minimum/maximum level, leaf budget, render scale, relief/skirt amplitude,
maximum render distance, horizon culling, wireframe, and LOD/face/SSE debug
views.

`Save settings` and `Load settings` persist the camera, presentation, LOD, and
patch inspection controls to the versioned local file
`helios_planet_lod_settings.json`. Loads are schema-checked and validated before
they can replace the live policy.

Live telemetry includes FPS, frame p95/p99, CPU LOD and submission time, GPU
duration-query time when supported, selected/visible/culled leaf counts,
triangles, draw calls, split/merge churn, per-level SSE, the adapter name, and
Diligent validation diagnostics.

The application reads only ordinary focused-window keyboard input. It never
enables SDL mouse grab, relative mouse mode, a global input hook, or screen
capture. When the route is paused: W/S changes altitude, A/D longitude, Q/E
latitude, Space toggles the route, R restarts it, and Escape exits.

## The planet as a system

`planet_surface.*` holds everything that describes a body's surface (the
height field, the climate that chooses its materials, its colours, its
atmosphere's scattering coefficients, or a gas giant's bands) and evaluates
the same height function the GPU draws (`shaders/lib/planet_noise.slang`) in
double precision for the camera and the level of detail.  `planet_system.*`
puts a body together with its rings, its cloud layer and up to four moons:
orbits from the planet's mass, ring gaps from the moons' resonances, the
Roche limit and Hill radii.  Named presets (Earth-like, desert world, ice
world, ringed gas giant) are the deep-sky lab's planet variants; that lab
draws the full system with its atmosphere, clouds and rings
(`examples/deep_sky_lab/README.md`).  This lab draws the terrain alone.

## Honest v0.0.1 boundaries

- Relief is a height field with the statistics of real terrain, evaluated
  in direction space; it is not a baked geophysical dataset and not a
  simulation of the processes that shape real surfaces.
- Skirts hide cracks; cross-face neighbour balancing and index stitch variants
  remain follow-up work.
- The selector is deterministic and budgeted, but v0.0.1 has no asynchronous
  GPU terrain residency or occlusion culling in this executable.
- The SDL-to-Diligent native-window bridge is Win32-only for this release.

The target is `helios_planet_lod_lab`. Build it normally, then launch it yourself
when an interactive GPU window is appropriate; automated tests must not start
the interactive route.

`helios_planet_lod_lab --capture PATH.png [--width N --height N --route-seconds S]`
renders one frame of the scripted route on a windowless device (no SDL window
or swap chain) and writes it as a PNG, with the leaf, triangle, screen-space
error and validation-error counts on stdout. It exits non-zero if the Vulkan
validation layer reported an error.

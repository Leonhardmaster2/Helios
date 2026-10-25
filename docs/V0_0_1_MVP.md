# Helios v0.0.1 MVP contract

Status: implementation branch `codex/v0.0.1-mvp`  
Target backend: Diligent Engine, Vulkan only  
Reference platform for the first release: Windows x64

Implementation checkpoint (31 August 2026): every target in this document compiles in the
Windows RelWithDebInfo build and all ordinary headless tests pass. The bounded Diligent precision
dispatch also passed its numeric limits with zero validation errors; it reported one warning, so
the warning text is now captured in its JSON and must be classified on the next opt-in GPU run.
The interactive planet executable has deliberately not been launched yet, so visual route,
coverage, driver-validation, and frame-time release gates remain pending rather than inferred
from a successful compile.

Implementation checkpoint (4 October 2026, uncommitted working tree): the interactive planet
executable was launched for the first time on the reference GPU. That run found one Vulkan
validation error (the patch vertex shader declared the SPIR-V `DrawParameters` capability, which
the Diligent device does not enable), dark seam lines where the deliberately darkened skirts
showed through patch gaps in the natural view, and an unreadably small UI at a 150-200 % display
scale. All three are fixed; an offscreen `--capture` route now reproduces route frames without a
window and reports zero validation errors. A real-time black-hole renderer,
`helios_black_hole_realtime`, was added and the CPU reference lab was moved onto the same
Page-Thorne black-body disk model. Its float32 geodesics agree with the binary64 reference on
every compared pixel (figures below). The complete interactive planet route with coverage,
frame-time and driver acceptance is still a pending release gate, and the evidence bundles have
not been regenerated because the tree is uncommitted.

This document defines what Helios v0.0.1 is allowed to claim. It deliberately separates
scientific correctness, numerical precision, rendering quality, and production readiness.
A striking image is not by itself evidence that the underlying model is correct.

## Release purpose

v0.0.1 is a technical demonstration of one engine operating across very different scales:

1. inspect a six-face cube-sphere and its quadtree decisions;
2. approach an airless procedural planet continuously from astronomical distance to metres
   above the surface;
3. inspect deterministic terrain generation and bounded tile residency independently;
4. run a separately scoped Schwarzschild black-hole reference laboratory;
5. capture reproducible performance, precision, and validation evidence.

The release is an MVP, not a finished Solar-System simulator. It does not claim real Earth,
production streaming, a validated atmosphere, collision physics, GRMHD plasma, or Kerr
space-time.

## Executables

### `helios_planet_lod_lab`

Interactive Diligent/Vulkan cube-sphere inspector and orbital-to-surface descent demo. Keeping
the selector and route in one application makes every route frame inspectable with the same UI;
the deterministic terrain/residency proof remains a separate project.

Required controls and evidence:

- orbit, altitude, vertical field of view, and route pause/scrub;
- split and merge screen-space-error thresholds;
- minimum/maximum LOD and hard leaf budget;
- impostor/globe/terrain representation thresholds and forcing;
- face, LOD, SSE, bounds, wireframe, and budget-limit diagnostics;
- current FPS/CPU/GPU time, rolling p50/p95/p99/max, draw/triangle/leaf counts, and
  per-level histogram;
- Vulkan/Diligent warning and error counts.

#### Scripted descent mode

The planet lab starts with a continuous scripted route from distant-body scale to local terrain.
The route uses logarithmic waypoints rather than a constant-speed camera:

```text
1e9 m distance -> 1e8 m -> 2e7 m -> 1000 km altitude -> 100 km -> 10 km
-> 1 km -> 100 m -> 10 m -> 2 m
```

The body stays at a frozen simulation instant while route time advances independently. The
camera may transition between root and body-fixed storage frames, but each frame must preserve
the existing precision rule: subtract origins in binary64 before converting bounded,
camera-relative values to binary32.

Acceptance conditions:

- no loading screen or uncovered planet pixels during the route;
- representation changes use hysteresis and an overlap/cross-fade region;
- parent coverage remains drawable until all required children are ready;
- stationary near-surface motion remains measurable below the published jitter threshold;
- all configured CPU, resident-tile, staging, request, and upload budgets remain hard limits;
- the route emits a deterministic JSON report and reference captures.

### `helios_solar_system`

The Sun and the eight planets on the cube-sphere renderer. The Sun is a body
(`PlanetBody.star = true`), not a light: an emissive, limb-darkened, granulated photosphere drawn
by the same patch renderer as the planets, with a screen-space limb fade. Planets use J2000 mean
elements and a two-body Kepler solver (`solar_system_data.hpp`); each body owns a quadtree selector,
spins at its sidereal rotation period, and is rebased to its own centre before the float32 upload.
The lab has an interactive orbit camera, a windowless `--capture` route, a hidden-window
`--smoke-frames` route, orbit/label overlays, and a 2x supersampled scene resolved by an exact 2x2
integer-load box filter. Acceptance for v0.0.1: `--capture` exits 0 with zero validation errors and
draws all nine bodies. Saturn's rings, moons, axial tilts, perturbations, and ephemeris-grade
accuracy are follow-up work; the element table is a visualization aid, not navigation data.

### `helios_terrain_lab`

Standalone deterministic generator and residency proof. This project is intentionally allowed
to evolve ahead of integration into the descent demo.

Required views and outputs:

- height, normal, slope, parent-compatible height, residual, and geometric error;
- seed, generator version, tile key, and octave controls;
- shared edge/corner inspection;
- requested/generating/resident/fallback/retiring state;
- repeatable content hash and a provenance record.

### `helios_diligent_precision_lab`

Bounded, no-window Diligent/Vulkan compute proof of the large-coordinate handoff. It constructs
binary64 world-space fixtures from planetary distance through one light-year, subtracts the
camera origin on the CPU, uploads only bounded camera-relative binary32 values, performs a view
transform in one eight-thread compute dispatch, and compares the readback with a binary64
reference. Its JSON output also shows the failure of casting absolute world coordinates to
binary32 before subtraction. The physical-GPU check is opt-in and never belongs to the ordinary
CPU-only CTest suite.

### `helios_black_hole_lab` and `helios_black_hole_realtime`

Separate scientific-rendering experiment. The v0.0.1 physics tier is explicitly:

> Schwarzschild (`a* = 0`) null-geodesic lensing in a stationary, isolated, uncharged vacuum,
> with an optional idealized thin test disc.

That is an exact general-relativistic space-time under its stated assumptions. It is not an
EHT/M87 simulation and it is not a Kerr black hole.

`helios_black_hole_lab` is the headless binary64 CPU reference. `helios_black_hole_realtime` is
the Diligent/Vulkan renderer of the same model: an interactive window by default, plus
windowless `--capture`, `--benchmark` and `--validate` routes that create no window or swap
chain. Required controls and evidence for the real-time lab:

- camera distance, elevation, azimuth, look offset and vertical field of view;
- disc outer radius, peak temperature, opacity and the illustrative turbulence amplitude;
- independent switches for space-time curvature, the Doppler term, and the gravitational
  redshift/time-dilation term, so each contribution can be inspected;
- sky mode (procedural stars, celestial grid, black), exposure, glare and tone map;
- quality presets and the underlying step, image-order and sample counts;
- current FPS, rolling frame p50/p95/p99, GPU trace and post-pass time, warning/error counts;
- a `--validate` report comparing every pixel-centre ray with the CPU reference.

### `helios_deep_sky_lab` and `helios_galaxy_probe`

Seven objects in one menu, none of them painted (`examples/deep_sky_lab/README.md`).
Each has named variants after classes of real objects, and a global quality
preset (Performance, Balanced, Ultra) sets every object's cost knobs at once:

- **Galaxy**: particle-mesh N-body run of 1.1 to 2.7 million star and gas
  particles with star formation and feedback; arms, bar and dust lanes grow
  from a smooth disk. Star colours come from a stellar-population sum over a
  Kroupa mass function. `helios_galaxy_probe` runs the same simulation on the
  CPU alone and reports rotation curve, mode amplitudes and star formation.
- **Emission nebula** and **supernova remnant**: compressible gas dynamics on
  the GPU (MUSCL reconstruction, HLLC/HLL, two-stage Runge-Kutta, compute
  shaders) on a grid that is finest at the centre and stretches towards the
  edges, so the box is several times wider than the fine region. `--validate-gas`
  checks the solver against Sod's shock tube and the Sedov-Taylor blast wave.
- **Neutron star** and **pulsar**: exact Schwarzschild light paths around a
  magnetised rotating star in one of seven states (cooling, pulsars, magnetar,
  accreting), with beamed outflow, striped wind, accretion streams and an
  irradiated disk, in visible light or the X-ray band.
- **Black hole**: the black-hole lab's renderer in the same menu.
- **Planet**: a planetary system on the planet lab's cube sphere: a height
  field with the statistics of real terrain, a single-scattering atmosphere
  with a precomputed transmittance table, volumetric clouds, rings whose
  gaps sit at the moons' resonances, and moons as bodies of their own on
  Kepler orbits. Presets: Earth-like, desert world, ice world, ringed gas
  giant.

The windowless routes (`--capture`, `--benchmark`, `--validate-gas`) create no
window and no swap chain.

## Planet topology and LOD

The planet is a radially projected cube sphere. A tile is identified by:

```cpp
CubeFace + level + integer x + integer y
```

The canonical sample path converts a tile/sample address to one rational integer face grid
before normalization. Shared samples therefore have the same pre-normalized representation on
both sides of a tile edge; even child samples exactly reproduce their parent samples.

The selection metric is:

```text
SSE_px = geometric_error_m * viewport_height
         / (2 * tan(vertical_fov / 2) * conservative_distance_m)
```

Nodes split above `split_threshold_px` and merge below the lower
`merge_threshold_px`. The previous selection is retained between these thresholds. Decisions
are sorted deterministically, respect the maximum level and hard leaf budget, and expose the
reason for every budget-limited result.

The full terrain renderer additionally requires one-level neighbor balancing, cross-face
neighbor transforms, parent-consistent morphing, and edge index variants. Skirts may only be a
temporary residency safeguard; they are not the topology solution.

## Terrain generation and residency

v0.0.1 terrain is a procedural height field; in the planet LOD lab it is airless, and the
deep-sky lab's planet adds the atmosphere, clouds, rings and moons described in
`examples/deep_sky_lab/README.md`. Height is evaluated from the normalized planet
direction, never independently in six face-UV domains. This keeps the generator continuous at
cube seams.

The generator contract contains:

- immutable seed and generator version;
- level-aware octave band limiting;
- a one-sample halo or equivalent cross-tile normal evaluation;
- min/max height and a conservative parent-relative error;
- parent-compatible low frequencies plus child detail residual;
- stable content hash over recipe and samples;
- finite-value and published-range checks.

Residency is a frame-boundary state machine. Render code consumes an immutable snapshot. Worker
or GPU completions become visible only during the next update. Request order is deterministic;
parents remain fallback coverage until children are drawable; memory and upload budgets are
never advisory.

## Rendering and UI architecture

Diligent is pinned as a recursive Git submodule. Its static Vulkan backend is the only backend
built for the MVP. Direct3D, OpenGL, Metal, and WebGPU are disabled. Helios keeps its offline,
pinned Slang-to-SPIR-V pipeline and passes embedded bytecode to Diligent; runtime shader
compilation is not part of the release path.

The pre-migration raw-Vulkan renderer remains a temporary parity target. It and Diligent each
contain a Vulkan loader implementation and must never be linked into the same executable.
Every MVP lab owns exactly one Diligent device, immediate context, and optional swapchain.

ImGui is used instead of Qt because it can share the render loop and existing SDL3 input path.
Every SDL event is offered to ImGui first. Scene/camera input is suppressed while ImGui requests
mouse or keyboard capture.

The UI settings model is applied at a frame boundary. Long-lived render/LOD work may not read
partially edited values. Settings must be saveable as JSON before release.

## Performance evidence

Every interactive lab reports:

- instantaneous FPS, CPU frame time, and GPU frame time;
- rolling p50, p95, p99, maximum, minimum, and mean;
- CPU update, LOD selection, terrain generation, and scene assembly where applicable;
- GPU terrain/depth/UI/total passes where timestamp support is available;
- leaves, requests, resident tiles, draws, vertices/triangles, and memory;
- validation/debug warning and error counts;
- resolution, internal scale, VSync state, quality settings, device, driver, and commit.

Cross-machine comparisons use the captured environment and settings. Cross-GPU floating-point
images use declared tolerances, not bitwise equality. Provisional 1080p targets on the reference
machine are p95 GPU below 16.6 ms and p95 CPU LOD below 2 ms; these are not release guarantees
until a complete reference route has been captured.

## Schwarzschild laboratory physics

The CPU reference integrates null geodesics in binary64. Rays are traced backward from a local
static-observer camera and terminate at the horizon guard surface, escape sphere, idealized thin
disc, or a published numerical limit. Diagnostics include step counts, closest approach,
impact parameter, affine length, and null-constraint residual.

Analytic validation points are:

- horizon `r = 2M`;
- photon sphere `r = 3M`;
- Schwarzschild ISCO `r = 6M`;
- distant critical impact parameter `b = 3 sqrt(3) M`;
- weak-field deflection approaching `4M/b`;
- static gravitational-redshift fixtures;
- scale invariance when all lengths and `M` are scaled together.

For a disc emitter, the observed/emitted frequency ratio is:

```text
g = (-k dot u_observer) / (-k dot u_emitter)
```

Liouville invariance gives spectral intensity scaling by `g^3` (with the emitted spectrum
sampled at the shifted frequency), while bolometric intensity scales by `g^4`. Exposure and
tone mapping happen after HDR radiance accumulation.

Both labs implement this. The disc follows the Page-Thorne zero-torque radial flux for
`a* = 0`, and for gas on circular geodesics seen by a static camera the ratio above has the
closed form

```text
g = sqrt(1 - 3M/r) / [ sqrt(1 - 2M/r_camera) * (1 - Omega * L_z/E) ],   Omega = M^(1/2) r^(-3/2)
```

in which `L_z/E` is conserved along the ray and is therefore known at the camera. A black body
at `T` is observed as a black body at `g T`, so one table of Planck spectra integrated against
the CIE 1931 colour-matching functions supplies both colour and absolute luminance. The table
is built once on the CPU and shared by the reference lab and the shader. The default peak
temperature (5200 K) is far cooler than a real disc, which would look uniformly blue-white; it
is chosen so the radial and Doppler temperature structure is visible, and it is a control.

Vulkan ray-tracing cores trace straight BVH rays and do not integrate curved null geodesics,
so the real-time path integrates them in a shader. It is a full-screen fragment pass rather
than a compute dispatch because the screen-space derivatives of the traced result supply the
filter footprints for the sky and the disc. Each ray is reduced to its orbital plane and the
exact Binet equation `u'' = -u + 3u^2` (`u = M/r`) is integrated in float32 with RK4, with steps
that land exactly on the angles where the ray meets the disc plane. The authoritative
implementation remains the adaptive CPU-double path; optional shader FP64 is a capability-gated
verification profile that has not been needed.

Measured on the reference machine (RTX 5080, 4 October 2026, uncommitted tree):

- geodesic agreement, 320x180, default camera: 57,584 pixels compared, 0 class mismatches,
  escape-direction error median 2.0e-6 rad and maximum 6.5e-5 rad, disc-radius error median
  7.4e-6 M and maximum 3.9e-4 M; 16 pixels inside the excluded critical band;
- with turbulence, glare and sky off, a real-time capture of the CPU lab's default camera
  matches the reference image to a median of one 8-bit level over the lit pixels;
- GPU frame time is recorded by `--benchmark`. Indicative values taken while another GPU
  workload was running are well under one millisecond per megapixel at one sample per pixel;
  clean numbers belong in the regenerated evidence bundle.

The initial lab explicitly excludes:

- Kerr spin/frame dragging and charge;
- dynamic or binary space-times;
- GRMHD turbulence, synchrotron transfer, absorption, scattering, polarization, and jets;
- wave optics and Hawking radiation;
- disc self-gravity.

The dark feature is the black-hole shadow/critical-lensing region, not a rendered singularity.

## Physically based baking roadmap

The bake system is a versioned scientific-data pipeline, not merely a shader precomputation
folder. Every recipe includes typed units, input hashes/licenses, algorithm and generator
versions, compiler/backend/device information where relevant, output hashes, and validation
tolerances. CPU references and GPU bakers share the same recipe.

### Phase A: radiometry, spectra, and color

- choose and document the scene-radiance unit convention;
- ingest a provenance-backed solar spectrum and angular diameter;
- define spectral sampling and spectral-to-XYZ/working-RGB conversion;
- publish working/output primaries, white point, transfer function, pre-exposure, and tone map;
- add false-colour luminance, NaN/Inf, negative-radiance, and energy-budget diagnostics.

### Phase B: surface light transport

- Cook-Torrance microfacet BRDF with declared normal-distribution, masking-shadowing, and
  Fresnel models;
- measured dielectric/conductor parameters where data exists;
- white-furnace energy-conservation and reciprocity tests;
- texture provenance, physical units/ranges, and mip/filter validation.

### Phase C: atmosphere

- transmittance, single/multiple-scattering, and irradiance LUTs;
- Rayleigh, Mie, ozone, solar spectrum, ground coupling, and finite planet shadow;
- dimensional/unit checks and CPU full-spectral comparisons;
- LUT slice goldens, order/resolution convergence, and energy checks;
- physically based exposure rather than hand-tuned sky brightness.

### Phase D: celestial-body material models

- airless-body photometry and thermal emission;
- catalog-derived stellar radiance/colour with distance and exposure handling;
- provenance-backed albedo, roughness, normal, displacement, and uncertainty;
- rings and gas giants only as separately documented and validated models.

### Phase E: offline reference and release evidence

- spectral CPU reference renders;
- GPU-versus-reference numeric and image comparisons;
- resolution, exposure, LUT, and step-size convergence;
- cross-GPU tolerance profiles;
- immutable EXR/KTX2 outputs plus JSON manifests and source licences.

## Scientific references

- Eric Bruneton, real-time Schwarzschild beam tracing and reference implementation:
  <https://ebruneton.github.io/black_hole_shader/paper.pdf>
- Page and Thorne, relativistic thin-disc flux: <https://articles.adsabs.harvard.edu/pdf/1974ApJ...191..499P>
- Luminet, Schwarzschild thin-disc image/flux reference:
  <https://articles.adsabs.harvard.edu/pdf/1979A%26A....75..228L>
- DNGR Kerr cinematic reference: <https://arxiv.org/abs/1502.03808>
- EHT general-relativistic radiative-transfer code comparison:
  <https://authors.library.caltech.edu/records/05cx0-5tq46>
- Bruneton precomputed atmospheric scattering implementation and validation:
  <https://github.com/ebruneton/precomputed_atmospheric_scattering>

## Release gates

v0.0.1 is ready only when:

- a clean recursive checkout configures without using a sibling absolute path;
- Diligent reports a Vulkan device and offscreen readback matches the commanded result;
- shader manifest/embedded-byte ABI tests pass;
- all CPU topology, LOD, terrain, residency, precision, and Schwarzschild tests pass;
- each advertised executable builds and its bounded automated route completes;
- the visual route has no coverage holes and produces zero validation errors;
- performance and limitations are visible in-product and captured in reports;
- docs and binaries identify the exact Helios and Diligent commits.

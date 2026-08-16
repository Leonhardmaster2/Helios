# Roadmap

Last updated: 16 August 2026. Current milestone: **M0 — implementation landed; evidence bundle in progress** (see `evidence/m0/README.md`).

This document owns delivery order and progress. [docs/PRODUCT.md](docs/PRODUCT.md) owns product scope, [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) owns intended implementation, and [docs/VALIDATION.md](docs/VALIDATION.md) owns proof.

**Milestone 10 is Helios v1.** Everything below the Milestone 10 gate is a future candidate, not part of the v1 estimate or promise.

Progress key: ⬜ not started · 🟨 in progress · ✅ validated. “Implemented” work does not become ✅ until its evidence bundle passes.

## Planning assumptions

- Part-time solo project unless maintainership changes.
- One reference desktop platform first; other platforms stay experimental until they pass the same suite.
- Every milestone emits a reproducible artifact and evidence, even if it is developer-facing.
- Effort ranges assume an experienced C++/Vulkan engineer and are for scope comparison, not calendar commitments.
- The Gaia Archive is the authoritative queryable upstream; v1 need not download the complete table and ships a bounded, reproducibly derived catalog.
- Water/ocean simulation, walking, gameplay, and celestial-body impact simulation are not v1 work.

| Milestone | Rough full-time effort |
|---|---:|
| M0 — skeleton and capability spike | 6–12 engineer-weeks |
| M1 — precision laboratory | 8–14 |
| M2 — cube-sphere revealed | 6–10 |
| M3 — first procedural planet | 8–14 |
| M4 — bounded continuous descent | 14–24 |
| M5 — surface-scale view | 6–10 |
| M6 — one measured place | 8–16 |
| M7 — Solar-System stage | 10–18 |
| M8 — atmosphere and HDR | 12–20 |
| M9 — Gaia DR3 sky | 8–16 |
| M10 — v1 vertical slice | 8–16 |
| **Total** | **94–170 engineer-weeks** |

Re-estimate after M0, M2, M4, and M6 using actual throughput and discoveries.

## Phase 0 — Prove the foundation

### 🟨 Milestone 0 — Project skeleton and capability spike

> **Status note (16 August 2026):** the M0 implementation described below
> exists and the reference-platform evidence bundle is being assembled
> (unit tests, shader ABI tests, headless smoke, windowed smoke — all with
> zero Vulkan validation errors on the reference device). The milestone
> flips to ✅ only when the evidence checklist in `evidence/m0/README.md`
> is fully green, including CI execution and platform items that the current
> environment cannot run (marked BLOCKED there).

**Goal:** produce the first reproducible executable and decide whether the proposed toolchain/platform profile is viable before engine architecture spreads.

Includes:

- CMake presets and one-command configure/build/test flow;
- pinned immutable dependency revisions and an offline/cache strategy;
- SDL3 window/input and a Vulkan 1.3 capability report;
- native triangle, clear, depth, compute dispatch, upload, and timestamp smoke paths;
- pinned Slang offline compilation to SPIR-V plus reflection/ABI tests;
- `helios_core`, logging, config, debug names, Tracy, ImGui, and doctest skeletons;
- validation layers, ASan/UBSan where supported, formatting/static checks;
- swapchain-free `--headless-smoke` path (automation only, not a research batch product);
- one reference-platform CI build/test and experimental platform compile/smoke jobs where practical;
- versioned report and asset-manifest schema stubs.

Decisions closed here:

- reference OS/toolchain and candidate minimum GPUs;
- proposed 1080p quality/performance/memory scenario;
- mandatory descriptor/depth/timestamp limits and fallbacks;
- macOS/MoltenVK support tier after Slang→SPIR-V→MSL tests;
- determinism tiers: exact canonical versus backend-repeatable versus cross-device tolerance;
- candidate Earth hero region/data product and Gaia DR3 catalog-shape experiment.

**Proof:** a clean checkout produces a validation-clean triangle/debug UI, compute output, capability JSON, unit-test result, and swapchain-free smoke artifact on the reference platform. Experimental-platform results are labelled, not generalized.

### ⬜ Milestone 1 — Precision laboratory

**Goal:** validate frame-tagged coordinates and depth over the actual v1 spatial range before planet systems depend on them.

Includes:

- barycentric, body-fixed, tile-local, camera-relative, and local-tangent types with illegal cross-frame operations rejected;
- final abstract `Instant`/`FrameSnapshot` consumer shape backed by synthetic static/rotating frame providers until M7 supplies astronomical conventions;
- one explicit scene-assembly point for `double`→camera-relative `float32` conversion;
- reversed-Z with queried 32-bit float depth support and infinite-far projection;
- camera frame reparenting with hysteresis, preserving pose and available motion state;
- low-detail sphere/impostor transition experiment;
- scripted path from 10¹³ m to surface scale, including repeated body-frame entry/exit;
- numeric ULP/round-trip/error report and screen-space jitter capture.

**Proof:** the M1 evidence bundle reports local transform error, root placement error, and projected jitter separately; passes the accepted [validation thresholds](docs/VALIDATION.md); shows no frame-change discontinuity beyond the pixel budget; and has zero Vulkan validation errors.

Failure here can change the coordinate architecture without invalidating planet code, because planet code does not exist yet.

### ⬜ Milestone 2 — Cube-sphere revealed

**Goal:** prove planet topology and LOD selection without terrain-generation or streaming complexity.

Includes:

- six cube-sphere faces and a canonical integer sample mapping;
- quadtree split/merge using screen-space geometric error and hysteresis;
- maximum one-level neighbor difference and balancing pass;
- 33×33 diagnostic patches with shared index data;
- frustum and conservative horizon culling;
- face-edge/corner property tests and debug visualizations.

**Proof:** captures show face IDs, patch bounds, LOD/error colours, and neighbor relations; exhaustive/property tests find no mismatched canonical edges, inconsistent corners, coverage holes, or illegal neighbor configurations.

## Phase 1 — Prove one planet

### ⬜ Milestone 3 — First procedural planet

**Goal:** produce versioned terrain whose exact and tolerance-based reproducibility contracts are honest.

Includes:

- generator/version/tile/cache key schema;
- CPU reference path for small fixtures;
- GPU height generation with band-limited 3D planet-coordinate noise;
- exact canonical shared-edge inputs and parent-consistent low-frequency terms;
- normals and diagnostic slope/altitude material;
- explicit floating-point controls/backend profile and no nondeterministic write order;
- hash tests for exact products and error/normal bounds for cross-device float products;
- local swapchain-free tile-generation test command (not yet a supported batch product).

**Proof:** duplicate shared samples match exactly within one declared asset/backend profile (or across profiles only when generated by the exact-canonical tier); same-backend fixtures meet the declared repeatability tier; supported device profiles meet cross-device conformance tolerances; generated artifacts include complete inputs/tool hashes; incompatible backend caches cannot mix.

If strict cross-vendor height identity is still required after this spike, generation must use a specified integer/fixed-point kernel rather than hoping drivers agree on floating transcendental results.

### ⬜ Milestone 4 — Bounded continuous descent

**Goal:** integrate tile requests, IO/generation, lifetime, morphing, and culling without unbounded memory or route-breaking stalls. This is the largest coupled risk before content.

Checkpoint A — residency/lifetime:

- orthogonal request, GPU-residency, and per-frame selection-role state, including failed/cancelled work and stale-completion tokens;
- renderer-to-planet `TileResidencySnapshot` feedback, with planet outputting desired leaves/fallback relationships and renderer resolving actually drawable roles;
- request priority, deduplication, cancellation, retries, and stale-camera reversal;
- parent coverage until children are ready;
- bounded CPU/GPU caches and graphics-timeline-safe atlas-slot retirement;
- transfer→compute→graphics synchronization and queue-family ownership;
- serialized same-queue baseline before async overlap optimization.

Checkpoint B — continuous rendering:

- per-dispatch work bounds and a measured generation/upload budget;
- parent-consistent geomorphing and skirts only as residency safety;
- frustum/horizon culling and stable visible-patch list;
- cold/warm cache telemetry and adversarial camera paths;
- fallback behavior for slow IO, no useful independent compute queue, or memory pressure.

**Proof:** the orbit-to-valley route completes on the reference profile with hard resident-tile/RAM/VRAM caps, recorded p50/p95/p99/max frame times, no unsafe resource reuse, no visible coverage holes, and no workload growth proportional to maximum tree depth. The exact limits are fixed before marking this milestone ✅.

### ⬜ Milestone 5 — Surface-scale view

**Goal:** make the last kilometre stable and visually legible without pretending it is walking gameplay.

Includes:

- local tangent camera orientation within the body-fixed domain;
- multi-band procedural material detail without texture swimming;
- surface/horizon scale cues and debug measurements;
- centimetre-scale prescribed camera motions and stationary-camera jitter tests;
- an explicit policy for terrain penetration (allowed in free flight or prevented by a simple safety floor).

**Proof:** eye-height capture toward a curved horizon; local precision and pixel-jitter thresholds pass; material detail remains stable under camera motion; the UI calls this hovering/free flight, not standing or walking.

General surface collision detection/response, surface physics, locomotion, avatars, and vehicles are not in the active roadmap. A route-only altitude clamp is permitted and is not a collision system.

### ⬜ Milestone 6 — One measured place

**Goal:** demonstrate the complete “honest scale” data pipeline on one named Earth region.

Includes:

- exact DEM product/tiles, DOI, license/terms, citations, and hashes;
- horizontal/vertical datum, units, documented accuracy, no-data and water-mask policy;
- offline GDAL-based conversion into versioned cube-face tiles;
- borders/halos, mip pyramid, min/max, geometric error, quantization bounds;
- explicit split between measured elevation and procedural residual detail;
- documented global approach appearance: explicitly synthetic outside the hero region unless an exact manifested low-resolution Earth land/ocean/albedo product is selected, plus a recorded measured-to-synthetic boundary blend;
- hardened parsers, size/checksum validation, and reproducible asset manifest;
- runtime provenance overlay showing source resolution versus rendered sample spacing.

**Proof:** a recognizable region is compared to source/reference control points and imagery; bake round-trip/error thresholds pass; the release identifies every measured and procedural component; no claim implies 1 m measurement accuracy from a coarser source.

## Phase 2 — Build the Solar-System scene

### ⬜ Milestone 7 — Solar-System stage

**Goal:** add time, reference frames, major-body positions, and rotations before atmosphere eclipse tests depend on them.

Includes:

- split/two-part instant representation and pinned time-conversion data;
- exact analytic series variants for planets and Moon;
- explicit centre, axes, units, ecliptic→ICRF/ICRS transformations, and model provenance;
- named body rotation models and Earth simplifications;
- versioned body radii/shape/mass-parameter inputs used for rendering and eclipse geometry;
- proposed supported date interval (candidate: 1900–2100) tested at a grid of epochs;
- low-detail Sun, Earth, Moon, and major planets plus diagnostic orbit/frame views;
- out-of-range date behavior and per-body tolerance report.

**Proof:** positions and rotations match an independent reference backend/service within published per-body tolerances over the accepted interval; all inputs/models are versioned; the UI says model-derived rather than “exact real position.”

### ⬜ Milestone 8 — Atmosphere, eclipse, and HDR

**Goal:** integrate one Earth atmosphere preset with a finite, stable physical-range lighting/exposure pipeline.

Includes:

- documented Bruneton-style LUT implementation and Earth parameters;
- versioned solar radius/angular-size and spectrum/irradiance convention;
- one scene-buffer radiometric convention;
- pre-exposure, FP32 reductions/intermediates where required, and non-finite detection;
- histogram exposure with percentile policy, adaptation, cut/reset, and manual controls;
- tone mapping, output colour conventions, and false-colour luminance view;
- atmosphere/terrain/distant-body composition;
- scripted day, night, sunset, Sun-entry, camera-cut, and Solar-System occlusion/eclipses.

**Proof:** LUT/reference comparisons pass; all validated buffers remain finite; exposure settling/overshoot thresholds pass; no discontinuity appears at atmosphere/space transitions; eclipse geometry comes from M7 rather than a missing future subsystem.

### ⬜ Milestone 9 — Gaia DR3 sky

**Goal:** turn Gaia DR3 into a reproducible, bounded star-rendering asset rather than a monolithic database copy.

Includes:

- pinned `gaiadr3` archive/schema, exact ADQL or bulk-source recipe, and immutable archived-result/bulk-input hashes;
- selected fields, null semantics, astrometric/photometric quality policy, and source counts;
- canonical `source_id` ordering, units/rounding/duplicate/serialization rules;
- reference-epoch records with runtime proper-motion handling where fields permit;
- HEALPix/magnitude partitioning and a bounded bundled v1 catalog;
- conservative neighbor/motion bounds for high-proper-motion sources crossing HEALPix pages;
- compact CPU/GPU formats with source-ID traceability;
- documented G/BP/RP→display flux/colour approximation and energy-conserving PSF;
- pinned Gaia passband/zero-point/calibration inputs used by that approximation;
- named bright-star/constellation validation and an explicit supplement decision if Gaia omissions matter;
- required ESA/Gaia/DPAC credit, the official requested paper citations, and a release manifest.

**Proof:** a clean bake reproduces the catalog hash and counts; named directions and brightness ordering meet thresholds; density/tile-boundary tests pass; the sky remains stable across exposure/resolution; runtime memory/storage stay within the v1 profile.

Full 1.8-billion-source interactive streaming is not required for v1. The offline pipeline should leave room for it later.

### ⬜ Milestone 10 — v1 vertical slice

**Goal:** turn the validated systems into the first public Helios product.

Includes:

- interactive reference explorer with minimal discoverable controls;
- bundled, versioned scenario, data/catalog manifests, and preset benchmark route;
- frozen astronomical time for the default benchmark, or an explicit versioned scripted time track for any route that advances it;
- Solar-System context → Earth approach → atmosphere → measured hero region → 2 m hover;
- provenance, coordinate/LOD, validation, performance, and memory overlays;
- replay/capture metadata;
- reproducible package on one reference platform and explicit labels for all other platforms;
- complete notices, known limitations, install/storage requirements, and evidence bundle.

**Proof:** a new user can install and complete/replay the journey; the 30–60 s capture is reproducible from the route; all P-001…P-013 requirements pass on the reference profile; memory is bounded; all mandatory buffers are finite; there are zero Vulkan validation errors.

## Milestone 10 decision gate

Only after v1 ships does evidence determine the next program. Candidate directions are unordered and unscheduled:

- SPICE/DE ephemeris backend, moons, small bodies, and spacecraft trajectories;
- more measured bodies, regions, atmosphere presets, rings, and comets;
- deeper Gaia magnitude tiers and full hierarchical catalog streaming;
- stable embedding/API work if external users actually need it;
- local headless batch/research tools, with multi-node/cluster work evaluated separately;
- GPU n-body experiments, beginning with direct O(N²) and conservation tests;
- interstellar navigation after adding a galactic/sector/star-system coordinate tier;
- scripted tours, annotations, and educational packages;
- relativistic rendering experiments such as black-hole lensing;
- **celestial-body impact program, if promoted**, with explicit fidelity levels:
  1. scripted/kinematic impact visualization and momentum/energy diagnostics;
  2. approximate procedural crater, deformation, heating, ejecta, and debris effects, with a revision/event layer that invalidates affected terrain caches and derived metadata;
  3. only if justified, offline or research-grade self-gravitating material/hydrodynamic simulation with equations of state and fragmentation.

No water, ocean, weather-fluid, or general real-time fluid simulation is currently planned. Celestial-body impact mechanics are distinct from camera/surface collision and walking, which are also not currently planned.

## Change policy

- Moving a future item above the gate requires a product/decision update and a revised estimate.
- Splitting a milestone to publish smaller evidence artifacts does not change v1 scope.
- A failed spike may remove or narrow a promise; that is progress, not something to hide.
- Progress becomes ✅ only when the evidence bundle passes, not when the code first renders a screenshot.

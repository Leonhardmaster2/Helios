# Architecture

Status: **preliminary architecture — accepted direction; v0.0.1 subset implemented, full v1 unvalidated**.

This document describes the intended full-v1 structure. Core portions now exist, including the
module graph, precision handoff, cube-sphere LOD/terrain/residency foundations, Vulkan paths, and
experimental labs, but later astronomy/data/atmosphere systems remain proposed. Product scope
lives in [PRODUCT.md](PRODUCT.md), the implemented MVP in
[V0_0_1_MVP.md](V0_0_1_MVP.md), contested choices in [DECISIONS.md](DECISIONS.md), and acceptance
evidence in [VALIDATION.md](VALIDATION.md).

Architecture statements use these maturity labels:

- **Accepted:** current direction; change through the decision log.
- **Proposed:** concrete starting design expected to evolve during its milestone.
- **Future:** intentionally outside the active v1 implementation.

## Architectural goals

1. Keep large-coordinate truth in frame-tagged CPU types and create GPU-relative floats at one boundary.
2. Prove precision, topology, residency, and data provenance independently before polishing content.
3. Keep planet-domain decisions independent from Vulkan resource/lifetime details where practical.
4. Make offline and runtime formats versioned, bounded, and shared by their writers/readers.
5. Use the simplest implementation that preserves the validated contract; optimize only with measured evidence.
6. Distinguish exact deterministic products from tolerance-conformant floating output.
7. Expose enough telemetry that a route failure can be reproduced from manifests and device state.

## System context

Helios v1 has three product surfaces:

```text
                         ┌──────────────────────────────┐
Gaia DR3 / DEM / models ─▶ offline bake tools           │
                         │ versioned assets + manifests │
                         └──────────────┬───────────────┘
                                        │
                                        ▼
user/input ─▶ reference explorer ─▶ internal engine libraries ─▶ Vulkan device/display
                    │                   │
                    │                   └─▶ swapchain-free validation/smoke
                    └─▶ diagnostics, replay, captures, evidence
```

- The **reference explorer** is the downloadable v1 product.
- The **internal engine libraries** are source-level boundaries, not a stable public SDK/ABI.
- The **offline tools** produce the exact formats the runtime reads; large source datasets remain external.

## Proposed module graph

Static libraries plus the app. Dependencies point only toward entries listed in “Allowed internal dependencies.” Include paths and CMake targets enforce the graph. `helios_blackhole` is an experimental v0.0.1 laboratory dependency rather than a commitment to the v1 explorer scope.

| Module | Responsibility | Allowed internal dependencies | Must not know |
|---|---|---|---|
| `helios_core` | platform services, errors, logging, config, jobs facade, profiling, IDs, checked IO primitives | none | coordinates, planets, Vulkan scene concepts |
| `helios_math` | vectors/quaternions, frame-tagged positions/poses, transforms, geodesy, cube-face mapping, numeric utilities | `core` | Vulkan handles, asset files, body registries |
| `helios_assets` | versioned binary headers/schemas, manifests, checksums, bounded readers/writers, tile/catalog records | `core`, `math` | live Vulkan resources, UI, dynamic LOD policy |
| `helios_universe` | time, frame graph, ephemeris/rotation backends, body registry, Gaia runtime catalog index | `core`, `math`, `assets` | Vulkan, terrain residency, application controls |
| `helios_planet` | body-local cube-sphere quadtree, SSE selection, tile request policy, cache/domain state, generator descriptions | `core`, `math`, `assets` | Vulkan handles/queues/descriptors, barycentric model internals |
| `helios_blackhole` | CPU-reference Schwarzschild geometry, null-geodesic integration, thin-disk radiometry, black-body colourimetry, diagnostics, and validation fixtures | `core`, `math` | GPU handles, UI, Kerr/GRMHD claims |
| `helios_vk` | instance/device selection, queues, swapchain/headless device path, VMA resources, descriptors, sync, pipeline cache | `core` | bodies, coordinate frames, terrain semantics |
| `helios_render` | scene assembly boundary, render resources/passes, planet/experimental GPU draw, atmosphere, stars, exposure, telemetry | `core`, `math`, `assets`, `universe`, `planet`, `blackhole`, `vk` during parity migration | user workflow, source archive formats such as ADQL/GeoTIFF |
| `helios_app` | main loop, input/camera intent, scenario/replay, orchestration, debug/provenance UI, report/capture wiring | `core`, `math`, `assets`, `universe`, `planet`, `blackhole`, `render` | direct Vulkan resource/queue control; offline source-data parsing |

The app has no `helios_vk` link edge. It creates the SDL window and passes an opaque window/surface source through `helios_render::RendererCreateInfo`; `helios_vk` owns `VkSurfaceKHR`, WSI, and Vulkan lifetime. The Vulkan module may use SDL3 as an external surface adapter without creating an internal dependency on the app.

For the Diligent migration, the raw `helios_vk` implementation is an isolated legacy parity target. Diligent and `helios_vk` each bring a Vulkan loader and must not be linked into one executable. New MVP labs link `Diligent-GraphicsEngineVk-static` with every non-Vulkan Diligent backend disabled; the legacy app/tests remain separate until each parity gate is replaced.

Offline tools:

| Tool | Purpose | Libraries |
|---|---|---|
| `dem_bake` | selected GeoTIFF/DEM products → cube-face tile pyramid + manifest | `core`, `math`, `assets` plus GDAL in the tool only |
| `gaia_bake` | pinned Gaia DR3 query/bulk input → magnitude/HEALPix runtime catalog + manifest | `core`, `math`, `assets` |
| `asset_inspect` | validate/dump any Helios asset without starting the renderer | `core`, `math`, `assets` |
| `route_report` | summarize/compare benchmark evidence | `core`, `math`, `assets` or a separate report schema target |

`helios_sim` is a **future** library, not an empty v1 stub linked into every build. N-body and celestial-impact programs should define their own data/step contracts when promoted.

### Why this differs from the first sketch

- `helios_assets` is explicit so tools and runtime cannot drift into duplicate binary schemas.
- Planet CPU/domain logic does not own Vulkan handles. `helios_render` executes its GPU generation/draw plans.
- `helios_universe` owns frame relationships and astronomical models, not every `double` in the program. Planet code may hold a frame-tagged body-local tile origin.
- The renderer, not arbitrary call sites, owns the one double-to-camera-relative-float scene boundary.

### No ECS or general render graph in v1

About ten major bodies and a terrain hierarchy do not justify an ECS. A body registry with generational/index handles is sufficient. Handwritten passes and synchronization remain acceptable while the pass set is small and tested. Revisit only when profiling or actual lifecycle complexity supplies evidence.

## Core data contracts

Names are illustrative C++ concepts, not frozen syntax.

```text
FrameKind                 // compile-time domain tag where practical
FrameId                   // runtime identity within a kind (Earth-fixed, Mars-fixed, ...)
Instant { integer_or_large_day, fractional_seconds, time_scale }

BarycentricPosition     // double, Solar-System root
BodyFixedPosition       // double, tagged with body/frame ID
TileLocalPosition       // float or quantized local value, tagged with TileKey
CameraPose<Frame>       // position + orientation, optional velocity state

FrameSnapshot             // immutable transforms for one simulation instant/frame
PlanetLodInput            // camera/body/view/error/budget + previous residency snapshot
DesiredPlanetPlan         // desired leaves + fallback graph + TileRequest[] + debug facts
TileResidencySnapshot     // renderer completion/availability facts for the next update
NearRenderRecord          // bounded camera-relative metre offsets
DistantBodyRecord         // direction + angular radius + analytic depth/distance
RenderScene               // near/distant records + physical/display parameters

AssetManifest
TileKey / TilePayloadHeader
GaiaCatalogHeader / GaiaPageHeader
RouteManifest / EvidenceReport
```

Compile-time tags prevent cross-**kind** arithmetic such as barycentric minus tile-local. Earth-fixed and Mars-fixed positions can share a C++ kind, so each value also carries a runtime `FrameId`; ordinary subtraction is unavailable and an explicit checked same-frame difference rejects cross-ID use. Serialization uses fixed-width fields and versioned schemas; in-memory C++ layout is never assumed to be the file format.

M1 uses synthetic static/rotating frame providers behind the final abstract `Instant`/`FrameSnapshot` shape. M7 selects real time scales, astronomical conventions, analytic models, and rotation providers without replacing the M1 consumer API.

## Coordinates and precision

### Spatial domains

**Accepted v1 domains:**

1. **Solar-System barycentric root.** ICRF/ICRS-aligned convention selected by the ephemeris contract, metres, `double`, intended magnitude up to about 10¹³ m.
2. **Body-fixed.** One rotating frame per detailed body, distances roughly ≤10⁷ m. Terrain and surface-local camera state live here.
3. **Tile-local.** Vertices/samples relative to a tile origin, represented by local floats or quantized integers.
4. **Camera-relative render representation.** Per-frame `float32` offsets, not a persistent world frame.

A **local tangent basis** is an orientation/view basis inside a body-fixed domain, not another absolute position tier.

Genuine interstellar navigation needs a **future** sector/star-system hierarchy above the v1 root. At roughly nearest-star distance a single `double` has metre-scale spacing, so the word “interstellar” cannot be implemented by expanding a constant.

### Precision contract

The architecture does not promise sub-millimetre root coordinates. At 10¹³ m, binary64 spacing is 1.953125 mm. It promises:

- high local numeric resolution after positions are expressed body-locally;
- explicit model/placement error for distant body centres;
- one controlled CPU conversion from frame-tagged doubles to camera-relative floats;
- a separately measured screen-space jitter threshold.

Per frame:

1. `helios_universe` publishes an immutable `FrameSnapshot` for the chosen `Instant`.
2. The app supplies the camera pose and planet plans in tagged domains.
3. `helios_render::SceneAssembler` transforms object/tile origins into the camera's owning/storage frame in `double`.
4. For near/local geometry, it subtracts the camera position in that same frame, checks the representation bound, and casts only the small relative offset to `float32`.
5. For distant bodies, it computes direction, angular radius, and analytic depth/distance in `double`, then emits a `DistantBodyRecord`; it never adds a planet radius to a million-metre-ULP `float32` centre.
6. The GPU view transform for near records contains rotation only; no shader subtracts huge absolute positions.

### Camera frame changes

Entering/leaving a body's configured transition region uses hysteresis. Reparenting must transform position and orientation and, once present, linear/angular velocity. Tests cover both directions, repeated round trips, poles, high rotation rates, and boundary oscillation.

The transition distance is a measured tuning/configuration value, not universally “100× radius.” Sphere of influence has a physical meaning and should not be used as a loose rendering label unless the calculation actually follows it.

### Depth

**Accepted direction:** reversed-Z, infinite far projection, `GREATER`/`GREATER_OR_EQUAL`, and an altitude-dependent near plane.

The device profile queries a writable 32-bit floating depth attachment (`D32_SFLOAT` or an accepted 32-bit-float depth/stencil alternative). Distant bodies use their explicit direction/angular-radius representation with analytically consistent depth; at 10¹³ m, a raw binary32 centre has roughly million-metre spacing and is not a usable sphere origin. Transition error is validated in pixels/depth, not assumed invisible because a disc is small.

## Per-frame runtime sequence

```text
input/replay
  → advance/select Instant
  → universe evaluates frame snapshot + body model states
  → camera intent is expressed/reparented in a tagged frame
  → planet LOD consumes body-local camera + budgets + previous TileResidencySnapshot
  → planet emits desired leaves, fallback relationships, tile requests, and debug facts
  → renderer resolves the actually drawable roles and builds near/distant RenderScene records
  → terrain compute/uploads and graphics are recorded with explicit dependencies
  → submit/present (or swapchain-free validation target)
  → publish completion messages/TileResidencySnapshot for the next update
  → collect timestamps, memory/residency facts, validation/provenance report
```

Simulation/update data for one rendered frame is immutable after scene assembly begins. Async IO completion publishes messages/handles into the next update rather than mutating live traversal state unpredictably.

The default benchmark route freezes astronomical `Instant` while camera time advances. A scenario may instead provide a versioned scripted time track; wall-clock time never changes body/star state implicitly during a reproducible replay.

## Planet LOD

**Accepted direction:** cube-sphere quadtree per detailed body, 33×33 diagnostic/grid patches initially, screen-space geometric-error selection, maximum one-level neighbor difference, parent-consistent morphing, and bounded residency.

### Canonical topology

One integer-lattice mapping defines `face + level + integer sample → canonical cube direction → sphere direction`. The implementation specifies:

- face orientation and winding;
- edge adjacency/coordinate transforms;
- ownership at the 12 cube edges and 8 corners;
- tile and sample integer widths/maximum LOD;
- border/halo samples for filters and normals;
- parent/child correspondence;
- quantization and displacement conventions.

Adjacent faces cannot independently evaluate float UVs and hope to match.

### Selection output

`PlanetLodInput` contains body shape/radius, body-local camera, view/projection facts, viewport, split/merge thresholds, tile/memory/request budgets, and the prior immutable `TileResidencySnapshot`. The output contains **desired** logical leaves, fallback/morph relationships, prioritized tile requests, and debug/error values—no Vulkan resources. The renderer resolves which desired leaves or fallback parents are actually drawable and returns completion/availability facts for the next update through planet-owned DTOs, avoiding a dependency cycle.

Geometric-error values and conservative min/max height bounds needed for SSE/horizon culling are always-available CPU metadata. LOD correctness does not wait for a GPU payload to become resident.

CPU traversal and double-precision frustum/horizon tests come first. Indirect/GPU culling may consume the same plan later. Mesh shaders/CBT remain research backends after v1.

## Tile lifecycle and terrain generation

### Orthogonal state

Visibility is a per-frame role, not a monotonic resource state. Helios tracks three related axes:

```text
logical request: absent → requested → reading → decoded
                              ↘ failed / cancelled → retry or absent

GPU allocation: unallocated → uploading → generating → resident → retiring → unallocated

frame role: unused | desired-leaf | drawable-leaf | fallback-parent | morph-source
```

Every transition has an owner, cancellation/retry rule, error value, and telemetry. A logical tile, source payload, and atlas allocation are distinct. A resident tile can be unused for many frames and later become a fallback or leaf without re-entering IO.

Each request/dispatch carries the full `TileKey`, a request generation, and an allocation generation. Late IO or GPU completion is discarded if any token no longer matches, preventing stale work from marking a reused atlas slot ready. Camera reversal can cancel work that has not consumed irreversible resources. Required parent coverage remains available until the renderer can draw the desired children.

Atlas slots/resources enter `retiring` only when no longer needed by logical policy or any frame role, and return to the free list after the graphics timeline passes their final recorded use. Descriptor updates follow the frames-in-flight safety policy.

### Pass chain

```text
LOD request (CPU planet domain)
  → source page IO / procedural parameter preparation
  → staging + transfer (or same-queue copy fallback)
  → bounded generation dispatches:
       base measured/procedural field
       optional versioned residual passes
       normals + min/max/error products
  → token-checked completion publishes residency for the next snapshot
  → renderer assigns drawable child/fallback/morph roles; retirement is a separate policy
```

The generation budget is measured with timestamps, but each dispatch must also have a bounded worst-case size. A “1–2 ms budget” cannot preempt an oversized dispatch after it starts.

Independent compute/transfer queues are optimizations. Correctness and the minimum support profile use a serialized graphics/compute/copy path when queue families or hardware overlap do not help.

### Erosion and non-local processes

Processes whose support crosses tile borders need halos and a convergence/ownership design. v1 puts erosion in the offline terrain bake if used at all. Runtime procedural residuals cannot silently change previously versioned terrain.

v1 terrain is immutable for a scenario/generator manifest. A future impact-deformation program requires an event/revision layer in tile keys and save/replay state, plus invalidation/regeneration of affected children, parents, normals, bounds, and geometric-error metadata. It is not an extra render pass over immutable tiles.

## Reproducibility contract

“Same seed + tile ID” is incomplete. A tile/cache key includes:

- asset/output schema version;
- generator algorithm and parameter-schema version/hash;
- seed and tile ID;
- source manifest hashes;
- compiler/shader/optimization profile where output depends on it;
- backend/determinism tier;
- quantization/output format.

Three tiers are supported:

1. **Exact canonical:** specified integer/fixed-point products and bitwise hashes across declared profiles.
2. **Backend-repeatable:** bitwise output for one pinned compiler/backend/device class; duplicate shared samples inside that asset set must match exactly.
3. **Cross-device conformant:** topology matches and floating fields remain inside specified height/normal/seam tolerances, but bits may differ.

Tiles from different backend profiles are never mixed in one runtime cache unless their shared border representation belongs to the exact-canonical tier. The manifest/cache key enforces this scope.

No fast-math flag or source-language `precise` keyword can, by itself, promise cross-vendor bit identity forever. Vulkan explicitly does not guarantee pixel-exact output between implementations, and MoltenVK adds a translation backend.

## Data and file formats

`helios_assets` provides common headers/readers for:

- manifest/report schemas;
- terrain tile/page metadata and payloads;
- Gaia catalog/index/page data;
- scenario and replay/camera-route data;
- atmosphere LUT metadata;
- pipeline/shader ABI manifests where useful.

Every format has magic, schema version, explicit endian/unit/coordinate fields, checked counts/offsets, size limits, checksums, and unsupported-version errors. Readers are fuzzable. Cache writes use a temporary file plus validated atomic replacement.

Source-specific concepts stay in tools: runtime terrain readers do not parse GeoTIFF/GDAL, and the renderer does not parse ADQL/CSV.

See [DATA_AND_PROVENANCE.md](DATA_AND_PROVENANCE.md).

## Gaia DR3 catalog architecture

The Gaia Archive is the authoritative queryable upstream containing about 1.81 billion sources. v1 neither needs a local full-table copy nor puts it in one SSBO.

`gaia_bake` records the query/bulk recipe and creates:

- an archived query result or immutable bulk-input hashes plus the query/retrieval metadata;
- a release manifest with selected fields, filters, counts, epoch, required credit/requested citations, and hashes;
- HEALPix/magnitude pages with compact source records;
- optional richer CPU metadata/index for traceability;
- validation fixtures for named bright stars and density distributions.

`gaia_bake` canonicalizes rows by `source_id` and specifies units, null/NaN policy, rounding, duplicates, and byte serialization. It stores epoch-tagged source astrometry; it does not independently propagate stars. `helios_universe` owns time/direction propagation and page selection, while `helios_render` owns GPU page residency and PSF/flux drawing. Missing parallax/proper-motion/colour is represented explicitly; no subsystem invents a zero that means “measured zero.”

High-proper-motion sources can cross a static HEALPix boundary over the supported date interval. Page selection must use conservative motion bounds plus neighbor pages, or a versioned rebinning policy, so moved sources cannot disappear at a page edge.

## Vulkan and synchronization

`helios_vk` exposes:

- instance/physical-device/device selection with a complete capability report;
- reference and portability profiles;
- queue handles and family/capability facts without promising hardware concurrency;
- VMA-backed buffers/images and debug names;
- descriptor arena with queried limits and a named shader/layout fallback;
- pipeline/shader cache keyed to the correct device/driver identifiers;
- binary swapchain semaphores quarantined inside WSI;
- graphics/compute/transfer timeline counters where supported by the profile;
- two frames in flight initially.

It does not know tiles, stars, frames, or atmosphere. `helios_render` translates domain plans into resource operations and explicit barriers/semaphore dependencies.

M0 names every required descriptor-indexing subfeature actually used (for example non-uniform indexing by descriptor type, runtime arrays, partially-bound descriptors, variable counts, and update-after-bind). “Use smaller tables” is not assumed to emulate a missing shader capability; the fallback has its own bounded descriptor layout/shader variant.

No dedicated render thread initially. Main-thread scene orchestration/recording plus a small job-system facade is easier to validate; parallel recording arrives only if profiling shows a CPU bottleneck.

## HDR and atmosphere

The scene buffer uses one documented quantity/convention. Irradiance (W·m⁻²) and radiance (W·m⁻²·sr⁻¹) are not interchangeable labels.

**Proposed pipeline:**

- atmosphere/star/surface inputs converted under documented radiometric/photometric approximations;
- pre-exposed RGBA16F scene colour where safe;
- FP32 reductions and sun-adjacent/intermediate products where range requires;
- log-luminance histogram with a defined sample mask and percentile policy;
- exposure history with explicit camera-cut/time-jump/first-frame reset;
- finite-value reduction and false-colour luminance diagnostics;
- swappable tone map with documented output primaries/transfer function;
- SDR first, HDR-display output a separately validated profile.

Bruneton LUT slices are compared to the chosen reference. “Physical intent” does not waive the need to document spectral-to-RGB approximations, atmosphere parameters, or error.

## Headless modes

The overloaded term is split:

1. **`--headless-smoke` (v1 foundation):** create a Vulkan device without a presentation swapchain, run bounded compute/render validation, and emit reports. Where a platform cannot do this, an explicitly labelled alternative is required.
2. **Local batch generation (later or tool-specific):** generate many tiles/catalog products without the explorer UI, with a supported CLI and resource controls.
3. **Cluster/multi-node (post-v1):** distribution, scheduling, fault tolerance, data movement, and result merging. Clean renderer interfaces do not make this free.

## Failure and fallback behavior

- Unsupported required device capability: fail before scene load with the missing feature/limit and support-tier link.
- No useful async queue: serialize on the graphics-capable queue.
- Tile/data read failure: retain parent/fallback representation, record the error/provenance, and keep memory bounded.
- Out-of-range astronomical date: reject or visibly switch to an explicitly documented extrapolation; never silently claim validated accuracy.
- Missing Gaia field: use the documented fallback representation or omit the source from that feature; never substitute a fabricated measurement.
- Insufficient memory: reduce a named quality/budget before allocation failure, or exit with a clear requirement.
- Device loss: capture available diagnostics and exit safely; live recovery is not a v1 requirement.
- Corrupt/incompatible cache: discard/rebuild after validation, never reinterpret bytes as a newer schema.

## Testing architecture

- `helios_math`: unit/property/fuzz tests for typed frames, transforms, cube faces, geodesy, ULP/error behavior.
- `helios_assets`: round-trip, malformed-input, size/overflow, checksum, and schema-version tests.
- `helios_universe`: time-scale fixtures, ephemeris/reference comparisons, body rotation, Gaia page/epoch semantics.
- `helios_planet`: LOD balancing, coverage, canonical edges/corners, priority/cancellation, cache state properties.
- `helios_vk`: capability selection, synchronization/resource-lifetime fixtures, debug/validation smoke.
- `helios_render`: headless small images/buffers, finite HDR checks, LUT references, resource retirement, tolerance-based goldens.
- `helios_app`: scenario/replay compatibility, report completeness, clean launch/install, full benchmark route.

GPU-dependent tests publish the device manifest. CPU reference paths cover small exact fixtures so CI without a suitable GPU still validates domain logic.

## Resource budgets

The architecture requires budgets but does not invent final values before profiling. M0/M4/M9/M10 must set and publish:

- maximum logical/resident/requested tiles;
- terrain atlas and staging bytes;
- per-frame generation/upload work and worst dispatch size;
- Gaia bundled/on-disk/resident page counts and bytes;
- atmosphere LUT and HDR target bytes;
- transient/per-frame allocation cap;
- RAM, VRAM/unified-memory, cache, and install size;
- frame-time percentiles and maximum route stall.

Budgets are inputs to LOD/residency decisions, not merely telemetry after allocation.

## Open architecture questions

These are not silently decided by the diagrams:

- exact reference-frame convention and typed C++ API shape;
- body transition criteria and motion-state requirements;
- exact analytic ephemeris variants and two-part time representation;
- cube-face mapping, tile ID width, border/normal scheme, and maximum LOD;
- whether exact height generation uses integer/fixed-point everywhere or only canonical seams;
- atlas/page representation and descriptor-limit fallback;
- scene radiometric convention and star/atmosphere/surface unit conversions;
- Gaia DR3 v1 selection, compact record, magnitude paging, and bright-star supplementation;
- Earth DEM product/region and measured/procedural composition;
- reference platform/device and numeric/performance thresholds.

Milestones close these questions with tests and decision-log entries. Until then, the architecture is specific enough to guide spikes but not “fully worked out.”

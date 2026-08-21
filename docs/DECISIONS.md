# Decision log

This log records accepted directions, their tradeoffs, and what evidence can reopen them. It does not mean the choices are implemented or validated. Architecture detail lives in [ARCHITECTURE.md](ARCHITECTURE.md).

Decision status:

- **Accepted:** current direction.
- **Provisional:** enough to run a spike, but the named milestone must confirm it.
- **Superseded:** retained for history with a link to the replacement.

To reopen a decision, add evidence, state which requirement changes, describe migration cost, and propose a validation plan. Enthusiasm alone is not evidence.

## D-001 — Solar-System coordinates use frame-tagged doubles and local GPU floats

**Status:** Accepted · **Date:** 12 July 2026 · **Validate:** M1

**Decision:** v1 uses a Solar-System barycentric root, body-fixed frames, tile-local coordinates, and a camera-relative GPU representation. CPU world/domain positions use frame-tagged `double`; the renderer performs one explicit same-frame subtraction and casts the small relative result to `float32`.

**Why:** millimetre fixed-point barely spans 10¹³ m and does not solve interstellar scope. CPU doubles are adequate for body centres at Solar-System range, while body-fixed/local frames retain far more precision for surface work. Typed frames prevent semantically invalid arithmetic better than a comment saying “absolute.”

**Important limit:** binary64 spacing at 10¹³ m is 1.953125 mm, so this does not support a literal sub-mm root-position promise. Genuine interstellar navigation requires a future sector/star-system tier; it is not a constant change.

**Rejected for v1:** one global `float`, GPU FP64 world coordinates, one millimetre `int64` universe, or undocumented floating-origin shifts scattered through views.

**Revisit if:** M1 cannot meet the local transform and screen-space jitter contract, or interstellar navigation becomes an accepted product requirement.

References: Cozzi & Ring, *3D Engine Design for Virtual Globes* (relative-to-eye material); [VALIDATION.md](VALIDATION.md).

## D-002 — Reversed-Z with a queried 32-bit floating depth attachment

**Status:** Accepted · **Date:** 12 July 2026 · **Validate:** M0/M1

**Decision:** use reversed-Z, an infinite far projection, `GREATER`/`GREATER_OR_EQUAL`, and a near plane chosen from camera context. Query `D32_SFLOAT` or an accepted 32-bit-float depth/stencil format rather than assuming one attachment enum.

**Why:** it preserves early/hierarchical depth behavior and provides a strong precision distribution for the camera-relative scene. A logarithmic fragment-depth path complicates depth-aware effects and writes without first proving reversed-Z inadequate.

**Consequence:** distant impostor/low-detail depth and near-plane changes need explicit transition tests.

**Revisit if:** a supported profile cannot provide the required float depth format or measured artifacts fail the v1 route.

References: [Vulkan depth format guidance](https://docs.vulkan.org/guide/latest/depth.html), Nathan Reed, *Depth Precision Visualized*.

## D-003 — Cube-sphere quadtree grid patches before research LOD backends

**Status:** Accepted · **Date:** 12 July 2026 · **Validate:** M2–M4

**Decision:** use a cube-sphere quadtree, initially 33×33 grid patches, screen-space geometric error, split/merge hysteresis, one-level neighbor balance, parent-consistent morphing, and skirts only as residency safety. Start with CPU traversal/culling.

**Why:** the algorithm is understandable, property-testable, compatible with tiled DEM assets, and achievable for a solo project. CBT/meshlet/mesh-shader schemes add implementation/debugging risk before the core data/residency model is known.

**Consequence:** face/corner topology, neighbor transformations, and error metadata must be completely specified; a cube-sphere is not seam-free automatically.

**Revisit if:** M4 profiling shows CPU selection/draw submission is a material bottleneck after simpler indirect-draw improvements.

Reference: Strugar, *Continuous Distance-Dependent Level of Detail for Rendering Heightmaps* (2009).

## D-004 — Vulkan 1.3 is a capability profile, not a hardware-year promise

**Status:** Provisional · **Date:** 12 July 2026 · **Validate:** M0

**Decision:** start with Vulkan 1.3, dynamic rendering, synchronization2, timeline semaphores, buffer device address, a declared descriptor-indexing minimum, timestamp queries, and a writable float depth format. Query every feature/limit and publish a device report.

**Why:** this keeps the native backend modern without adopting Vulkan 1.4 solely for version novelty. The required features align with the renderer direction.

**Consequence:** “2019 or newer” is removed as a support claim. M0 may reduce a requirement if a bounded fallback preserves the product and materially improves hardware coverage.

**M0 evidence (2026-08-16):** every proposed hard requirement was probed and enabled on the reference device (dynamic rendering, synchronization2, timeline semaphores, buffer device address, host query reset, shader draw parameters, D32_SFLOAT, timestamp queries — see evidence/m0/local/capabilities.json); the smoke suite runs with zero validation errors. Descriptor-indexing bits are reported but M0 shaders do not yet require them (declared minimum recorded in evidence).

**Revisit if:** capability spikes exclude intended reference devices or the renderer does not actually need a proposed hard requirement.

## D-005 — Slang shaders compiled offline to SPIR-V

**Status:** Provisional · **Date:** 12 July 2026 · **Validate:** M0/M3

**Decision:** use pinned Slang binaries/source revisions, offline compilation, checked-in build integration, reflection/ABI tests, and explicit optimization/float profiles. Do not check generated SPIR-V into source unless release/offline-build needs justify it.

**Why:** modules, generics, and multi-target tooling suit a compute-heavy renderer better than a growing preprocessor convention.

**Consequence:** Helios owns another toolchain dependency. macOS adds SPIR-V→MSL translation through MoltenVK, so M0 must exercise actual shaders, not just compile a trivial file.

**Fallback:** documented GLSL/glslang path only if Slang blocks supported platforms or reproducible builds; a theoretical fallback is not maintained in parallel for free.

**M0 evidence (2026-08-16):** slangc v2026.14.1 compiles all M0 shaders offline to SPIR-V; the reflection/ABI tests verify entry points, descriptor bindings, block offsets and push-constant layout against the CPU ABI header; shader bytes are embedded at build time (self-contained binaries) and their manifest hashes are verified. MoltenVK SPIR-V→MSL behavior remains unvalidated (blocked: no macOS environment).

**Revisit if:** pinned releases cannot produce stable ABI/reflection behavior or MoltenVK translation prevents required shaders.

Reference: [Slang project](https://github.com/shader-slang/slang).

## D-006 — Analytic ephemerides first; SPICE later behind explicit contracts

**Status:** Provisional · **Date:** 12 July 2026 · **Validate:** M7

**Decision:** begin with an exact selected VSOP87 variant for major planets and an exact selected lunar series/backend, behind an interface that names centre, axes, time scale, supported interval, and uncertainty/tolerance. Add SPICE/DE only after v1 unless validation proves the analytic backend inadequate.

**Why:** a bounded analytic backend is easier to distribute and exercise for the v1 visual stage. SPICE adds kernel discovery/versioning, global-state boundaries, time/frame inputs, and more operational surface than “read planet coordinates.”

**Consequence:** “any date” is rejected. The chosen series variants, transforms (including ecliptic/ICRF differences), two-part time representation, leap-second inputs, body rotations, and per-body tolerances are M7 deliverables.

**Revisit if:** the accepted date/tolerance contract cannot be met, or spacecraft/small-body trajectories become the next post-v1 program.

References: [NAIF SPICE time subsystem](https://naif.jpl.nasa.gov/pub/naif/toolkit_docs/C/req/time.html), [JPL DE440/DE441 ranges](https://ssd.jpl.nasa.gov/doc/de440_de441.html).

## D-007 — Gaia DR3 is the authoritative v1 star source

**Status:** Accepted · **Date:** 12 July 2026 · **Validate:** M0/M9

**Decision:** replace HYG with ESA Gaia Data Release 3 as the upstream catalog. v1 ships a reproducibly selected, versioned Gaia-derived catalog organized by magnitude and HEALPix; it does not place all ~1.81 billion sources in one resident buffer.

**Why:** Gaia DR3 provides far more extensive astrometry and photometry, rich quality/provenance fields, and open/free use with required ESA/Gaia/DPAC credit. The archived [official HYG repository](https://github.com/astronexus/HYG-Database) and [license snapshot](https://raw.githubusercontent.com/astronexus/HYG-Database/main/LICENSE), checked 12 July 2026, publish CC BY-SA 4.0 rather than public-domain terms.

**Consequence:** M9 becomes a real data-pipeline milestone: exact ADQL/bulk recipe, quality filters, missing fields, reference epoch, compact format, counts/hashes, photometric approximation, bright-star validation, and attribution/citations. Gaia is not optimized for the very brightest stars, so a separately reviewed supplement remains an explicit decision if validation needs it.

**Rejected:** an unversioned download, a hand-edited star list, all-source resident SSBO, or silently substituting missing astrometry/colour with measured zeros.

**Revisit if:** Gaia archive/redistribution terms or bright-source completeness cannot support the v1 sky, or a later full-galaxy streaming product needs another source model.

References: [Gaia DR3 documentation](https://gea.esac.esa.int/archive/documentation/GDR3/), [credit instructions](https://gea.esac.esa.int/archive/documentation/GDR3/Miscellaneous/sec_credit_and_citation_instructions/).

## D-008 — Adopt focused libraries; do not outsource the renderer architecture

**Status:** Provisional · **Date:** 12 July 2026 · **Validate:** M0 and per dependency

**Decision:** candidates are volk, vk-bootstrap, VulkanMemoryAllocator, SDL3, Dear ImGui/ImPlot, Tracy, enkiTS behind a thin facade, doctest, KTX-Software, meshoptimizer, ERFA, and GDAL in offline tools only. Pin immutable revisions and record licenses/notices.

**Why:** rebuilding platform, allocation, UI, profiling, image, time-standard, and geospatial foundations adds risk without differentiating Helios. A general renderer/framework would obscure the Vulkan/planet architecture the project exists to explore.

**Consequence:** no dependency is approved merely because it appears in this paragraph. M0 verifies maintenance, license, platform, binary/runtime, offline build, and transitive dependency implications.

**Rejected initially:** a custom allocator/job system/test framework, runtime GDAL, an ECS framework, or a package-manager policy that fetches mutable branches.

**Revisit if:** a candidate's actual license/platform/maintenance cost fails the dependency review.

## D-009 — No ECS, render thread, general render graph, or custom job system until evidence

**Status:** Accepted · **Date:** 12 July 2026 · **Validate:** M4 profiling

**Decision:** use explicit body/tile structures, handwritten pass/synchronization code, main-thread orchestration/recording, and a small job-system facade.

**Why:** these abstractions have real payoff at sufficient scale and equally real upfront/debug cost. v1 has a small body set and a bounded pass plan.

**Consequence:** handwritten synchronization still needs a precise resource-state/lifetime model and tests. “No render graph” is not permission for scattered barriers.

**Revisit if:** measured pass/resource dependency complexity or CPU recording time becomes a dominant problem.

## D-010 — v1 is a surface-scale rendering explorer, not walking or simulation

**Status:** Accepted · **Date:** 12 July 2026 · **Validate:** M10

**Decision:** v1 ends with a free-flight camera roughly 2 m above one Earth terrain region. It has no avatar, vehicle, general collision detection/response, walking, gameplay, fuel/orbital dynamics, n-body evolution, water, or celestial-impact simulation. A benchmark-only altitude clamp is allowed.

**Why:** each excluded item adds another product and content/physics/interaction validation surface. The rendering transition is already a multi-year project.

**Consequence:** user-facing language says “hover” or “camera,” never “standing.” The camera may need a simple safety-floor policy for the benchmark, but that is not a general collision engine.

**Revisit if:** v1 ships and evidence/users justify a separately scoped program.

## D-011 — Plan AGPL-3.0-only plus commercial licensing, but do not claim operations before legal readiness

**Status:** Provisional legal/business policy · **Date:** 12 July 2026 · **Validate:** before accepting code contributions

**Decision:** repository-owned code is intended to remain available under AGPL-3.0-only, while a rights-complete commercial license may be offered for proprietary uses. External code is accepted only under a counsel-reviewed contributor agreement granting the necessary relicensing rights.

**Why:** this aims to preserve an open edition while allowing proprietary customers to fund development.

**Important correction:** contributors retain copyright under the draft CLA; the needed thing is consistent relicensing/sublicensing rights, not “100% copyright consolidation.” Third-party libraries and datasets are not magically dual-licensed by Helios.

**Consequences:**

- The legal identity, contact, agreement, privacy/record process, company-contribution path, and signature workflow must be complete first.
- Plain-language AGPL trigger/derivative-work guidance is fact-specific and must be reviewed by counsel.
- Helios-owned files cannot incorporate code whose rights do not permit the intended commercial license. This is a commercial-relicensing policy, not a claim that GPL and AGPL can never interact under their own terms.
- `AGPL-3.0-only` is the selected community identifier. Any later open license would be additional rather than silently changing existing files to “or later.”

**Revisit if:** counsel, contributor adoption, distribution channels, or customer evidence shows the model is unworkable.

## D-012 — Determinism has exact and tolerance-based tiers

**Status:** Accepted · **Date:** 12 July 2026 · **Validate:** M3

**Decision:** exact cross-platform bit identity is promised only for canonical products implemented with specified exact operations. Floating GPU output is backend-repeatable or cross-device tolerance-conformant according to a declared profile. Generator/data/tool/backend versions are part of every cache key.

**Why:** Vulkan does not guarantee pixel-exact output across implementations, floating controls do not cover every operation, drivers may contract/approximate differently, and MoltenVK translates SPIR-V to MSL. “No fast-math” is necessary but insufficient for “every machine forever.”

**Consequence:** seams/topology can remain exact while nonessential heights/normals use validated error bounds. If product requirements demand exact heights everywhere, use an integer/fixed-point canonical generator and pay its performance/algorithm cost knowingly.

**Revisit if:** M3 demonstrates a broader exact profile on all supported backends with a specified algorithm, or tolerances visibly break caching/research requirements.

References: [Vulkan invariance](https://docs.vulkan.org/spec/latest/appendices/invariance.html), [float-controls limitations](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_shader_float_controls2.html).

## D-013 — “Honest scale” is the product identity

**Status:** Accepted · **Date:** 12 July 2026 · **Validate:** M6/M9/M10

**Decision:** the explorer and asset formats classify important content as measured/observed, model-derived, procedural/synthetic, or display transform. The active source/model/generator manifests remain inspectable.

**Why:** large-scale rendering and open space viewers already exist. Visible provenance unifies Helios's real-data, reproducibility, extensibility, and education/research intentions into a concrete differentiator.

**Consequence:** “1 m geometry” cannot be marketed as “1 m terrain accuracy”; model positions need date/frame/tolerance; exposure and false colour cannot be confused with source data.

**Revisit if:** never for honesty itself; implementation/UI details may evolve with usability evidence.

## D-014 — One reference platform earns support first

**Status:** Provisional · **Date:** 12 July 2026 · **Validate:** M0/M10

**Decision:** choose one reference OS/toolchain/device profile in M0. Linux, Windows, and macOS can have build/experimental coverage, but only profiles passing the full validation route are called supported.

**Why:** driver, packaging, shader, queue, and performance differences are significant. Equal three-platform expectations before one renderer exists multiply uncertainty.

**Consequence:** macOS/MoltenVK receives an explicit portability spike. A successful compile or triangle does not imply v1 support.

**M0 evidence (2026-08-16):** the reference profile is Windows 11 + MSVC 2022 on an NVIDIA RTX 5080 (D-017). Linux and macOS CI build jobs are configured; macOS/MoltenVK stays experimental until the shader translation and device suite run there.

**Revisit if:** automation/hardware/contributor capacity makes another profile cheap to validate continuously.

## D-015 — Tools and runtime share versioned asset schemas

**Status:** Accepted · **Date:** 12 July 2026 · **Validate:** M0/M6/M9

**Decision:** add `helios_assets` for bounded binary schemas, manifests, checksums, and readers/writers used by tools and runtime. Source-specific formats stay offline.

**Why:** the DEM and Gaia pipelines are part of the product's correctness. Independent ad-hoc readers/writers would drift and make old caches unsafe.

**Consequence:** every format has versioning, explicit units/coordinates/endian, size limits, hashes, malformed-input tests, and migration/rebuild policy.

**Revisit if:** schemas naturally split into more focused libraries after real formats exist; never return to duplicated structs serialized by `sizeof`.

## D-016 — Celestial impacts are a post-v1 fidelity ladder; water simulation is not planned

**Status:** Accepted scope clarification · **Date:** 12 July 2026 · **Validate:** post-M10 proposal

**Decision:** do not plan water/ocean/weather fluid simulation. If celestial-body impacts are promoted after v1, scope them in explicit levels:

1. scripted/kinematic impact visualization with momentum/energy diagnostics;
2. approximate procedural crater, deformation, heat, ejecta, and debris effects;
3. only if justified, offline or research-grade coupled self-gravity/material/hydrodynamics using named methods, equations of state, and conservation validation.

**Why:** “impact mechanics” can mean a visual event or a planetary-material simulator; their cost and claims differ by orders of magnitude. Calling all collision “fluid simulation” obscures the choice.

**Consequence:** celestial impacts do not imply camera/surface collision, walking, general rigid-body gameplay, or water. Even tier 2 needs a terrain event/revision layer, cache-key changes, save/replay provenance, and invalidation/regeneration of affected children, parents, normals, bounds, and geometric-error metadata; it is not a render-pass toggle. The highest-fidelity tier is a separate multi-year research program.

**Revisit if:** M10 has shipped and a concrete fidelity/use-case proposal includes data, compute, validation, and product scope.

## D-017 — M0 reference platform: Windows 11 + MSVC 2022 (NVIDIA RTX 5080)

**Status:** Accepted for M0 · **Date:** 2026-08-16 · **Validate:** M10

**Decision:** the M0 reference OS/toolchain is Windows 11 (build 26200) with
MSVC 19.44.35228 (VS 2022 17.15, C++20), CMake 4.2.0-rc2, Ninja/MSBuild, the
Vulkan SDK 1.4.328.1, and an NVIDIA GeForce RTX 5080 (driver 610.74, Vulkan
1.4.341) as the reference device. The full M0 suite — unit tests, shader ABI
tests, headless smoke with validation layers, and the windowed SDL3/swapchain
path — was executed on this profile with zero Vulkan validation errors
(evidence: `evidence/m0/local`, `evidence/m0/perf`).

**Why:** the M0 evidence bundle requires an environment where the complete
suite actually runs. This machine is that environment; the capability probe
reports every required feature and limit.

**Important limit:** the reference profile is a *development* profile. The v1
minimum GPU profile (candidate: any Vulkan 1.3 discrete GPU with the required
features and ≥6 GiB VRAM, per [SUPPORT.md](SUPPORT.md)) still needs the device
matrix; macOS/MoltenVK remains experimental until the Slang→SPIR-V→MSL
translation tests run on macOS hardware (blocked in the M0 environment).

**Revisit if:** a hardware/CI resource appears that can validate a second
profile continuously.

## D-018 — M0 determinism tier policy for runtime products

**Status:** Accepted · **Date:** 2026-08-16 · **Validate:** M3

**Decision:** M0 formalizes the three determinism tiers from
[ARCHITECTURE.md](ARCHITECTURE.md) as schema-tagged categories in every
cache/asset key:

1. **exact-canonical** — specified integer/fixed-point products, bitwise hashes
   across declared profiles (M3 height generation at shared seams);
2. **backend-repeatable** — bitwise output for one pinned compiler/backend/
   device class (M0 evidence: the compute smoke kernel is verified bit-exact
   against the CPU reference on the reference device);
3. **cross-device-conformant** — floating outputs within declared per-field
   tolerances, seams exact (M0 evidence: depth probe values read back at
   exactly 0.150/0.500/0.900 within ±0.01 on the reference device).

The `helios-asset-manifest` schema carries a `class` field (measured /
model-derived / procedural / display) and every compiled artifact records its
generator + SHA-256, so mixed-tier caches can be detected and rejected.

**Revisit if:** M3 shows the accepted tier boundaries do not match GPU
behavior on the device matrix.

## D-019 — M0 Gaia DR3 catalog-shape experiment

**Status:** Accepted (shape); **data selection pending** · **Date:** 2026-08-16 · **Validate:** M9

**Decision:** the v1 bundled catalog shape is: HEALPix level 6 pages
(≈0.92° resolution), G-magnitude tiers (G≤6 bright tier, 6<G≤10, 10<G≤12),
compact 24-byte GPU records, plus a richer CPU metadata page for provenance
traceability. The record layout and page format are defined in
`helios_assets` (M9 will finalize exact fields) and the build-time
determinism rules (canonical `source_id` ordering, units, null policy) are
specified in [DATA_AND_PROVENANCE.md](DATA_AND_PROVENANCE.md).

**Evidence from M0:** the schema plumbing, bounded page/manifest formats and
their round-trip/malformed-input tests exist in `helios_assets`; a
magnitude-tier page count estimate for the bundled subset (G≤12 with quality
filters ≈ a few hundred thousand sources, well within the v1 storage profile)
was derived from Gaia DR3 documentation counts. The exact ADQL query and the
archived query-result hash require Gaia Archive access and are therefore
**not closed by M0** (see Remaining M0 work).

**Revisit if:** the real query count or bright-star completeness changes the
page/bundle sizes by more than 2×, or high-proper-motion tests find sources
crossing page boundaries.

## D-020 — Candidate Earth hero region / data product

**Status:** Candidate only — **decision open** · **Date:** 2026-08-16 · **Validate:** M6

**Decision:** M0 records the candidate profile, not a commitment: one exact
SRTM/NASADEM 30 m product tile group over a mid-latitude region with a
checkerboard-free interior (e.g., an Alps or Rocky-Mountain 3°×2° window), with
vertical datum, void policy, and product-page terms to be recorded before any
download. The license/terms and hash of the exact product must be documented
in the M6 selection record; M0 cannot responsibly close this because the
product terms and a controlled download/validation environment were not
available (see Remaining M0 work).

**Revisit if:** M6 validation shows the candidate region's voids or datum
conversion exceed the accepted error budget.

## D-021 � Frame-tagged type API: checked same-frame difference, no implicit cross-kind operations

**Status:** Accepted � **Date:** 2026-08-21 � **Validate:** M1

**Decision:** `helios_math` implements the M1 coordinate types as
`Position<KindTag>` (compile-time kind tag + runtime `FrameId`) with no
implicit conversions and no operator arithmetic between positions. The only
cross-position operation is `differenceTo()`, which returns a
`Result<Vec3d>` and fails with `InvalidArgument` when the runtime frame IDs
differ; translation is explicit (`displacedBy`). Orientations use
double-precision quaternions (`Quatd`); float32 appears only at the render
boundary. `helios_universe` publishes immutable `FrameSnapshot`s from an
abstract `FrameProvider`; M1 ships synthetic static/rotating providers with
all frames as direct children of the barycentric root, behind the same
consumer API that M7's real rotation models will implement.

**Why:** follows D-001 and ARCHITECTURE.md "Coordinates and precision".
Compile-time tags make cross-kind arithmetic a type error; the runtime
checked difference catches Earth-fixed vs Mars-fixed misuse that shares one
C++ kind. Result-based errors match the project's no-exceptions convention.

**Revisit if:** M1's transform-error/jitter evidence shows the checked API
is too costly in hot loops, or M7's real hierarchies need parent-relative
transforms that the flat snapshot shape cannot express.

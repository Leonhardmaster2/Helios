# Validation plan

Status: **accepted evidence policy; most numeric thresholds await Milestone 0/1 measurements**.

This document owns how Helios turns a promise into evidence. A requirement without a range, scenario, threshold, reference, and repeatable test is an aspiration, not validation.

## Evidence levels

| Level | Meaning |
|---|---|
| **Unit/property** | Small deterministic tests for math, mappings, parsers, and invariants |
| **Integration** | Several modules execute together with controlled inputs |
| **Route** | The versioned benchmark camera path exercises runtime transitions |
| **Reference** | Output is compared to an independent implementation, dataset, or analytic result |
| **Device matrix** | The same test is run on named OS/GPU/driver/compiler profiles |
| **Release evidence** | Reports, logs, captures, hashes, and manifests are published with the artifact |

Visual inspection is useful for discovery, but “looks fine” is not a numeric threshold.

## Required scenario manifest

Every route/performance capture records:

- Helios commit and dirty-worktree state;
- build type, compiler, Slang/SPIR-V tool versions, and dependency lock hash;
- OS, kernel/build, CPU, RAM, GPU, VRAM, and driver/runtime versions;
- Vulkan device/feature/limit report and MoltenVK/Metal profile where applicable;
- display resolution, refresh mode, quality preset, and output colour mode;
- body/data/catalog/generator manifests and hashes;
- epoch/time-scale inputs plus `frozen` or versioned scripted time-track policy;
- route file and hash;
- every validation reference input (imagery/control points, ephemeris service/kernel, LUT/reference implementation) with version or retrieval timestamp, hash/snapshot where possible, terms/license, and citation;
- warm/cold cache state and number of repetitions;
- validation-layer configuration;
- output report schema version.

Results missing the scenario manifest are anecdotes, not support evidence.

Reference imagery, catalog extracts, kernels, and comparison outputs enter an evidence bundle only when redistribution permits it. Otherwise the bundle records an immutable citation/hash or access recipe without copying restricted material.

## v1 requirements matrix

`TBD-M0` and `TBD-M1` are explicit gates: the corresponding milestone must choose and document the number before later work can claim success.

| Requirement | Claim | Test and reference | Pass condition |
|---|---|---|---|
| P-001 | Downloadable interactive explorer | Clean-machine install/launch test on the reference platform | Release starts without a developer toolchain; bundled scenario and controls are discoverable |
| P-002 | Seamless benchmark route | Cold- and warm-cache replay with event markers | No explicit scene reload, blocking loading screen, crash, or missing mandatory representation; streaming events remain within the recorded policy |
| P-003 | Local precision | Frame-transform/property suite and route capture | Same-body/body-local numeric error ≤ **0.1 mm proposed**; no discontinuity above `TBD-M1` pixels during frame changes; barycentric/model placement errors reported under a separate budget |
| P-004 | Terrain LOD | Exhaustive cube-edge tests, adversarial route, residency report | Duplicate shared samples are bit-identical within one declared asset/backend profile (or exact-canonical across profiles); no illegal >1 LOD neighbor; crack/topology threshold set in M2, memory/residency cap in M4, and near sample-spacing threshold in M5 |
| P-005 | Atmosphere/HDR | Reference LUT slices, day/night/sunset/occlusion scripts, buffer reductions | Reference difference threshold `TBD-M8`; zero NaN/Inf; exposure settles within documented time/overshoot bounds |
| P-006 | Solar-System placement | Epoch grid compared to an independent reference service/backend | Named interval, model, frame, and per-body position/rotation tolerances published and passed; out-of-range input is rejected or visibly qualified |
| P-007 | Gaia sky | Query/bake reproducibility, named-star directions/brightness order, density histograms | Catalog/query hashes reproduce; required bright stars present or supplement disclosed; direction/photometric thresholds `TBD-M9` |
| P-008 | Provenance | Manifest/UI inspection and release-package audit | Every measured/model/procedural/display component resolves to a complete manifest with no `TBD` |
| P-009 | Repeatability | Exact-hash and cross-device conformance suites | Integer/canonical products match hashes; floating products stay within declared per-field tolerances on supported devices |
| P-010 | Diagnostics | Automated capture/report schema check | Frame time, memory, tile state, coordinate/frame state, validation status, and manifest IDs are present and machine-readable |
| P-011 | Platform support | Clean build/install/run and full route on each claimed tier | “Supported” only after all mandatory checks pass; compile-only or smoke-only platforms remain labelled experimental |
| P-012 | API/HDR correctness | Validation layers, sanitizers, buffer finite checks | Zero Vulkan validation errors in the route; ASan/UBSan clean where supported; mandatory HDR/intermediate buffers contain no unexpected non-finite values |
| P-013 | Legal/release completeness | Package/source/notices/manifest audit | AGPL license and exact corresponding-source/build access are present; About/legal UI, third-party notices, required dataset credits, requested citations, manifests, and rights records are complete; no public placeholder remains |

The proposed 0.1 mm same-body/local transform threshold is tighter than the user-visible requirement and is intended to leave budget for later systems. It does not apply to barycentric model placement. M1 may change it only with a recorded rationale and an updated product claim.

## Coordinate and precision suite

### Scalar arithmetic checks

- Record `double` ULP at 10⁷, 10⁹, 10¹², and 10¹³ m.
- Test transforms with values near zero, tile boundaries, cube corners, body radii, sphere-of-influence thresholds, and 10¹³ m root positions.
- Reject or handle NaN, Inf, denorm-sensitive, and invalid frame IDs explicitly.

### Typed-frame properties

- Round-trip `A → B → A` position/orientation error under random valid frames.
- Composition associativity within a documented tolerance.
- Body-fixed↔barycentric transforms at multiple rotation phases.
- Camera reparenting in both directions with hysteresis.
- Thousands of repeated in/out transitions to detect accumulated drift.
- Preserve position and orientation; preserve velocity/angular velocity once those fields exist.
- Compile-time tests reject arithmetic across different frame **kinds** (for example barycentric versus tile-local).
- Runtime tests reject a checked difference between different `FrameId`s of the same kind (for example Earth-fixed versus Mars-fixed); ordinary unchecked position subtraction is unavailable.

### Screen-space stability

Run stationary and prescribed micro-motion cameras at the surface, transition boundary, orbit, 10⁹ m, 10¹² m, and 10¹³ m. Record maximum and percentile projected movement of static landmarks after removing intentional camera motion.

**M1 threshold (accepted 21 August 2026, measured in `evidence/m1`):**

- **Stationary camera, static landmark: 0 px** — a deterministic engine must
  produce bitwise-identical projections for identical inputs; any nonzero
  value indicates nondeterminism, not round-off.
- **Cross-representation jitter ≤ 0.5 px** — the deviation between projecting
  a landmark from barycentric storage versus through a body-frame storage
  round trip. Rationale: sub-pixel instability is invisible at normal viewing
  distances; 1 px is the first visibly detectable discontinuity, so the
  budget is set below it with headroom. Measured on the reference profile:
  ≤ 0.018 px worst case at 10¹³ m.
- **Frame-change discontinuity ≤ 1.0 px** — the visible-detection bound; the
  per-switch pose jump is additionally bounded at 1e-2 m in root coordinates
  (measured: 0 m — transitions are exact transforms of the same point).

## Depth suite

- Query and record support for `D32_SFLOAT` or the accepted 32-bit-float depth/stencil alternative.
- Validate the reversed-Z projection analytically at near, representative, and asymptotic distances.
- Test near-plane changes for discontinuities.
- Compare analytic/impostor depth to full sphere depth at transition angular sizes.
- Render overlapping near terrain, atmosphere, and distant bodies to find ordering regressions.

## Cube-sphere and terrain suite

### Topology/property tests

- Every face edge maps to the canonical neighboring edge in both directions.
- All eight cube corners resolve consistently.
- Neighbor balancing never leaves more than one LOD level difference.
- Split/merge hysteresis terminates and does not oscillate under a stationary camera.
- Parent and child coverage is complete with no duplicate ownership ambiguity.

### Height and normal tests

- Shared canonical integer height inputs produce exact matching samples.
- Parent low-frequency terms equal child low-frequency terms.
- Procedural residuals occupy only the documented frequency band.
- Normals and filtered samples meet a cross-edge angular/difference threshold.
- Quantized DEM round trips stay within the manifest's error bound.

### Residency state tests

Exercise logical request state, GPU allocation/residency state, and per-frame selection roles independently. Cover cancellation, retry, duplicate requests, failed reads, resident-but-unused reuse, fallback/morph parents, rapid camera reversal, and device loss where practical. Deliberately complete stale IO/compute work after a request/allocation generation changes and prove it cannot publish into the reused slot. An atlas slot cannot be reused until the graphics timeline passes its last use.

### Route/performance tests

Use cold and warm caches, slow and teleporting cameras, low-memory pressure, and intentionally slow IO. Record:

- requested/generated/drawn tiles;
- resident CPU/GPU bytes and peak allocations;
- dispatch and upload durations;
- p50/p95/p99/max CPU and GPU frame time;
- fallback/parent coverage duration;
- cache hit rate and eviction/cancellation counts.

M0 selects a candidate reference profile. M4 must set hard RAM, VRAM, and frame-time limits before claiming the descent is bounded.

## Determinism tiers

| Tier | Intended guarantee | Suitable implementation |
|---|---|---|
| **Exact canonical** | Bitwise equality across declared platforms/versions | Integer/fixed-point sample coordinates, hashes, explicitly specified operations |
| **Backend-repeatable** | Bitwise equality for one generator, compiler, device/backend profile | Carefully controlled floating GPU kernel, no nondeterministic writes, pinned toolchain |
| **Cross-device conformant** | Results differ only within a field-specific tolerance; seams remain exact | Portable floating operations with output error/normal bounds |

Every tile key includes generator schema/version, algorithm hash, seed/parameters, source-manifest hashes, tile ID, output format, and backend/determinism profile where relevant.

## Astronomy and time suite

- Use a split/two-part instant representation and test sub-second increments at large Julian dates.
- Test UTC/TAI/TT/TDB conversions at leap boundaries and ordinary epochs with a pinned data version.
- State what future or pre-leap UTC inputs mean.
- Compare each body at a grid covering the full supported interval, not only three convenient dates.
- Record reference backend/service version, frame, centre, aberration setting, and units.
- Validate body-fixed rotations independently from centre positions.
- Test out-of-range dates and missing time-data behavior.

The supported date interval is a product decision. `1900–2100` is a reasonable candidate for v1 validation, not an accepted fact until M7 records models and measured error.

## Gaia DR3 suite

- Store the exact source query/bulk recipe and hash the immutable bulk input or archived query result.
- Canonically sort by `source_id` and test null/NaN, units, rounding, duplicate, and byte-serialization rules.
- Count rows after each filter and compare against the release manifest.
- Round-trip source IDs and HEALPix partitioning.
- Validate a named list of bright/navigation stars and recognizable constellations.
- Check dense Milky Way fields, sparse fields, Galactic poles, and catalog tile boundaries.
- Compare aggregate magnitude and sky-density histograms with the source query.
- Test null astrometry/photometry fields and quality flags.
- Test high-proper-motion sources at HEALPix boundaries across the supported date interval so page selection cannot omit them.
- Confirm energy-conserving PSF behavior at different resolutions/exposures.
- Verify required ESA/Gaia/DPAC credit and the official requested scholarly citations are in the release package where applicable.

## HDR and atmosphere suite

- Compare precomputed LUT slices against the chosen reference implementation/CPU reference.
- Test camera below ground, at sea level, near atmosphere top, in space, and through limb views.
- Script sunrise/sunset, Sun entering a night-exposed frame, eclipse/occlusion, camera cut, and rapid time change.
- Reduce buffers to min/max/non-finite counts every validation run.
- Verify pre-exposure reset/history rules and histogram percentile behavior.
- Check that star PSFs conserve configured flux across output resolutions.
- Document scene-buffer units, output primaries, transfer function, and SDR/HDR-display path.

## Device and platform matrix

For each candidate device, publish:

- capability/limit probe;
- shader compilation and reflection tests;
- triangle + compute + transfer + headless smoke;
- full mandatory test suite when promoting to supported;
- benchmark route results for at least three repeated warm runs and one cold run;
- known deviations and fallback paths.

MoltenVK results record both Vulkan/MoltenVK and Metal/macOS versions. A successful build is not proof of equivalent rendering or performance.

## Release evidence layout

The exact binary/report formats arrive with M0, but each milestone artifact should expose a structure equivalent to:

```text
evidence/<milestone>/<commit>/<device-id>/
  scenario.json
  capabilities.json
  test-results.xml
  validation.log
  performance.json
  memory.json
  manifests/
  captures/
  README.md
```

Large captures may live in release storage, with immutable links and hashes in the repository.

## Failure policy

- A failed threshold makes the claim unvalidated; it does not get hidden as “driver variance.”
- If one platform fails, reduce its support tier or add a documented fallback.
- If the design cannot meet a product threshold, update the decision and product contract openly.
- Golden images use perceptual/numeric tolerances where the API does not promise pixel identity.
- Evidence is preserved when it disproves an assumption; failed spikes are valuable roadmap results.

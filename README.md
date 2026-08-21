<p align="center">
  <img src="assets/banner.png" alt="Helios" width="100%">
</p>

# Helios

> **Status — Milestone 0 implementation landed, 16 August 2026.** The repository now contains a C++20/CMake source tree with unit tests, a pinned offline Slang→SPIR-V pipeline with reflection/ABI tests, a Vulkan capability report, swapchain-free and windowed smoke paths that run with **zero Vulkan validation errors** on the reference profile, and the first performance evidence. See [ROADMAP.md](ROADMAP.md), [evidence/m0/README.md](evidence/m0/README.md) and the BUILDING section below. Every technical claim in this document beyond M0 evidence remains a target.

Helios is a planned open-source, real-time **Solar-System visualization engine and reference explorer**. Its first public release aims to move a free-flight camera from an interplanetary view to roughly two metres above one detailed planetary surface without a scene-loading transition.

It is intended to be written in C++20 and Vulkan. The community edition is planned for AGPL-3.0-only, with a separately negotiated commercial license for Helios-owned code once the legal and operational pieces are ready.

## The 60-second explanation

The first release, called **v1** in these documents, is exactly the vertical slice delivered by [Roadmap Milestone 10](ROADMAP.md). It is meant to be an interactive technical explorer and a reproducible benchmark, not a game and not yet a general-purpose SDK.

A v1 user should be able to:

1. launch a bundled Solar-System scene at a documented date;
2. follow a preset Earth-approach route or control the camera directly;
3. travel from the planetary context, through the atmosphere, to an eye-height hover above terrain;
4. inspect overlays for coordinate frames, level of detail, performance, and data provenance; and
5. replay the same camera path to compare builds and hardware.

Earth is the accepted v1 **hero body**: one still-to-be-selected curated region uses a documented elevation dataset, while detail below the source resolution is visibly identified as procedural. The Sun, Moon, and other planets provide the astronomical stage and are not promised the same terrain detail in v1.

“Seamless” means that the route does not stop at an explicit scene-loading screen. Terrain and other resources will still stream in the background. “Two metres above the surface” describes a camera position, not a walking character: v1 has no general collision detection/response, avatar, vehicle, fuel, orbital gameplay, vegetation, buildings, or water simulation. A route-only altitude clamp may keep the benchmark camera above terrain without becoming a surface-physics system.

The longer-term vision may grow toward more bodies, interstellar navigation, research tooling, and embeddable engine APIs. Those are separate programs of work, not hidden v1 promises.

## Why build it?

Existing space and geospatial viewers prove that large-scale navigation is useful. Helios's proposed identity is **honest scale**:

- **Measured data stays measured.** A source region records its dataset, datum, native resolution, transformations, and uncertainty.
- **Model-derived astronomy stays model-derived.** Ephemerides and body rotations name the model, reference frame, supported date range, and tested tolerance.
- **Procedural synthesis is visible.** Generated detail is never described as measurement merely because it looks plausible.
- **Display transforms stay display transforms.** Exposure, tone mapping, and artistic color do not change the provenance of the underlying data.

The engine is also intended to be modular internally. Stable third-party plugin or ABI compatibility is not a v1 commitment; that requires real interfaces and a compatibility policy, not just separate static-library names.

## v1 contract

| Area | v1 target | Important limit |
|---|---|---|
| Product | Interactive desktop reference explorer plus a scripted benchmark route | Not a game or full engine SDK |
| Spatial range | Solar-System / interplanetary view to a camera about 2 m above one surface | Interstellar travel is post-v1 architecture work |
| Precision | Sub-millimetre **local numeric resolution** near the surface and a separately measured screen-space jitter budget | A single barycentric `double` at 10¹³ m has about 1.95 mm spacing, so “sub-mm everywhere” is not claimed |
| Terrain | Continuous cube-sphere LOD with roughly 1 m near-camera sample spacing on the hero body | Sample spacing and synthesized detail are not 1 m measurement accuracy |
| Real terrain | One named Earth DEM region with full provenance | Global, uniformly high-resolution measured terrain is not promised |
| Lighting | One Earth atmosphere preset, physical-light conventions, HDR exposure, and tone mapping | Scientific radiative-transfer accuracy is not implied |
| Astronomy | Sun, Earth, Moon, planets, and a catalog-derived star sky over a documented date interval | “Any date” and mission-grade navigation accuracy are not promised |
| Repeatability | Versioned inputs and deterministic seams; exact hashes where integer generation permits, tolerances otherwise | Floating-point GPU output is not promised bitwise-identical across every vendor forever |
| Platforms | One reference platform first; other platforms earn support through the same validation suite | A GPU's release year is not a capability or performance guarantee |

The measurable contract and still-open thresholds live in [docs/VALIDATION.md](docs/VALIDATION.md).

## Is this realistic?

The core vertical slice is technically credible. The from-scratch implementation is still a large rendering, data, numerical, tooling, and release-engineering project. A rough planning estimate for an experienced C++/Vulkan engineer is **94–170 full-time engineer-weeks**, before ongoing maintenance. At roughly half-time, with integration, specialist review, and rework, **four to seven calendar years** is a more credible expectation than a quick solo release.

That estimate is not a promise. Milestones 0–4 exist to retire the risks that could materially change it. See [docs/FEASIBILITY.md](docs/FEASIBILITY.md) for the claim-by-claim assessment and assumptions.

## Technical direction

The current proposal uses:

| Problem | Proposed direction |
|---|---|
| Large coordinates | Frame-tagged `double` positions on CPU; one camera-relative `float32` scene handoff to the GPU |
| Depth | Reversed-Z, an infinite far projection, and a queried 32-bit floating depth format |
| Planet LOD | Cube-sphere quadtree, 33×33 grid patches, screen-space-error selection, morphing, and bounded residency |
| Rendering API | Vulkan 1.3 capability profile, with serialized fallbacks when independent compute or transfer queues do not help |
| Shaders | Pinned Slang compiler, offline SPIR-V compilation, reflection/ABI tests, and a MoltenVK compatibility spike |
| Astronomy | A simple analytic ephemeris backend first, later replaceable by SPICE behind the same frame/time contract; a versioned Gaia DR3-derived star catalog |
| Terrain | Versioned, data-driven generation passes; GPU compute only where its determinism and portability contract is explicit |

Read [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the intended module and runtime design, and [docs/DECISIONS.md](docs/DECISIONS.md) for decisions and revisit triggers.

## Repository guide

There is deliberately no `src/`, `CMakeLists.txt`, or build command yet.

| Start here | What it answers |
|---|---|
| [docs/INDEX.md](docs/INDEX.md) | Which document is authoritative, and what should I read next? |
| [docs/PRODUCT.md](docs/PRODUCT.md) | What is the product, who is it for, and what does v1 actually do? |
| [ROADMAP.md](ROADMAP.md) | In what order will it be built, and what proves each milestone? |
| [docs/FEASIBILITY.md](docs/FEASIBILITY.md) | Which expectations are realistic, risky, or not credible as written? |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | How is the implementation intended to be divided? |
| [docs/VALIDATION.md](docs/VALIDATION.md) | How will precision, seams, performance, astronomy, and portability be tested? |
| [docs/DATA_AND_PROVENANCE.md](docs/DATA_AND_PROVENANCE.md) | Where do data and generated assets come from, and how are they labelled? |
| [docs/SUPPORT.md](docs/SUPPORT.md) | What does “supported hardware/platform” mean? |
| [docs/GLOSSARY.md](docs/GLOSSARY.md) | What do the specialist terms and acronyms mean? |
| [CONTRIBUTING.md](CONTRIBUTING.md) | What can be contributed at the current stage? |
| [LICENSING.md](LICENSING.md) | What is the intended licensing model and what is still unresolved? |

## Building and contributing

Milestone 0 is complete and Milestone 1 is in progress: the repository now
contains a full CMake build (MSVC 2022 on Windows; presets in
`CMakePresets.json`), a Vulkan 1.3 renderer with headless and windowed smoke
paths, doctest unit suites, and evidence tooling. See [ROADMAP.md](ROADMAP.md)
for current status, `evidence/m0/README.md` and `evidence/m1/README.md` for
verified results, and `evidence/m0/commands.md` for the build/test commands.

Documentation issues and corrections are welcome. Code contributions are not being accepted until the reviewed contributor agreement, real legal identity/contact details, and signing workflow exist. See [CONTRIBUTING.md](CONTRIBUTING.md).

## Licensing status

The repository states an AGPL-3.0-only licensing intent and contains a **draft, unreviewed** contributor agreement. The actual licensor(s), chain of title, and banner ownership must be named/confirmed before outsiders are asked to rely on that grant or any commercial terms. Third-party code, datasets, catalogs, and assets keep their own licenses. See [LICENSING.md](LICENSING.md); its plain-language notes are project guidance, not legal advice.

Copyright © 2026 the Helios repository authors. The complete public legal rights-holder notice is still a release blocker; see [LICENSING.md](LICENSING.md).

# Platform and hardware support policy

Status: **no supported build exists**. This document defines how a platform earns a support label once code exists.

“Vulkan 1.3” and “a mid-range GPU from around 2019” are not sufficient support specifications. A device can expose an API version while differing in optional feature bits, limits, queue mapping, depth formats, descriptor capacity, memory, driver quality, and performance.

## Support labels

| Label | Meaning |
|---|---|
| **Reference** | The primary development/release platform. Full mandatory tests and benchmark evidence gate releases. |
| **Supported** | Clean install plus full v1 validation suite passes on named hardware/software profiles; regressions are release blockers within policy. |
| **Experimental** | Expected to build or run and receives smoke coverage, but may have correctness, performance, packaging, or feature gaps. |
| **Build-only** | Compilation is checked; no claim that a suitable GPU/runtime path executes. |
| **Unsupported** | Not tested or known not to meet the profile. Community patches may still be considered after the contribution process opens. |

No platform is reference or supported today.

## Milestone 0 decision

Milestone 0 selects:

- one reference OS/toolchain/GPU profile;
- at least one named minimum GPU and driver for the eventual v1 target;
- a candidate display/performance profile;
- smoke coverage for other desktop platforms;
- whether macOS/MoltenVK is experimental, build-only, or promoted later.

Selection is evidence-driven. The maintainer's daily machine can be a development target without automatically being the minimum release profile.

## Proposed v1 Vulkan capability profile

The capability probe records exact API, extension, feature, limit, queue, memory, and format support. Requirements may be reduced during M0 if a simpler fallback preserves the product.

| Capability | Proposed status | Fallback / note |
|---|---|---|
| Vulkan 1.3 instance/device path | Required on native Vulkan targets | MoltenVK uses its advertised portability implementation/profile |
| Dynamic rendering and synchronization2 | Required | Core to the proposed backend; no legacy render-pass backend planned |
| Timeline semaphores | Required | Used for coarse queue/resource progress |
| Buffer device address | Required by proposed profile | Revisit if actual use does not justify excluding devices |
| Descriptor indexing/bindless limits | Required at a documented minimum, exact bits queried | Provide capped tables/batches where a device cannot support the preferred arena size |
| 32-bit floating depth attachment | Required: `D32_SFLOAT` or accepted 32-bit-float depth/stencil format | Query format features; do not assume a specific enum is writable everywhere |
| Timestamp queries | Required for performance evidence | Calibrate validity/period per platform; CPU estimates do not replace GPU timing |
| Multi-draw indirect + count | Optional until M4 profiling justifies it | CPU-driven direct draws are the correctness baseline |
| Graphics queue | Required | — |
| Compute-capable queue | Required, but not a distinct family | Serialize terrain compute on graphics when independent overlap is unavailable/unhelpful |
| Transfer-capable queue | Required, but not a distinct family | Use graphics/compute queue for copies when no dedicated transfer path helps |
| Async queue overlap | Optimization only | Never a correctness or fixed-budget assumption |
| Headless/swapchain-free device path | Required for smoke/benchmark automation where the runtime permits | A hidden window is not equivalent and must be labelled if used |
| Validation layers | Required on development/CI images, not end-user runtime | MoltenVK uses the Vulkan loader/layers path described by its SDK |

Feature promotion to core does not remove the need to enable/query features and limits correctly.

## Candidate performance profile

The initial target to test—not yet a promise—is:

- 1920×1080 output;
- one “balanced” quality preset;
- approximately 60 Hz presentation intent;
- a minimum discrete-GPU class around 6–8 GiB VRAM, named by exact model/driver;
- bounded RAM/VRAM and p50/p95/p99/max frame-time limits chosen after M0/M4 measurements;
- the canonical cold- and warm-cache route from [VALIDATION.md](VALIDATION.md).

The final profile must name exact GPUs rather than a year. Reasonable spike candidates include one NVIDIA, one AMD, and—if resources permit—one Intel device representing the intended floor. This list is not a purchase recommendation or support declaration. The M0 device matrix still needs AMD and Intel devices.

## Platform intentions

### Linux

Candidate reference platform because native Vulkan tooling and CI are straightforward. The actual distribution/compiler/window-system/driver matrix is chosen in M0. “Linux” never means every distribution or driver.

### Windows

Candidate additional v1-supported profile, or the reference platform if M0 selects it; it is not independently required for v1. Any promoted profile must name supported Windows versions, MSVC/Clang toolchain, GPU drivers, installer/portable packaging, and redistributables. It remains experimental until its full route passes.

### macOS via MoltenVK

Intended early experimental target, not assumed release parity. MoltenVK is a Vulkan portability implementation over Metal and converts SPIR-V to MSL. Its current guide documents capability-dependent requirements such as Metal argument-buffer support and descriptor-indexing limits.

The M0 spike must verify:

- SDL surface creation and portability enumeration flags;
- the selected buffer-device-address and descriptor-indexing profile;
- Slang → SPIR-V → SPIRV-Cross/MSL shader behavior;
- floating-point/determinism tier behavior;
- 32-bit float depth attachment and reversed-Z;
- timestamp reliability;
- swapchain-free smoke path;
- frame pacing and pipeline-cache behavior;
- validation and capture workflow.

If it fails the mandatory profile, macOS remains experimental with explicit fallbacks or is build-only. The architecture does not contort silently to preserve a three-platform sentence.

Official reference: [MoltenVK Runtime User Guide](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md).

## CPU, memory, and storage

Final limits are validation outputs, but every release profile must state:

- supported CPU architectures and instruction-set baseline;
- minimum/recommended system RAM;
- minimum/recommended VRAM or unified-memory budget;
- application binary/dependency size;
- bundled Gaia-derived catalog size;
- bundled terrain/atmosphere asset size;
- cache maximum and eviction policy;
- free disk space required for installation and first run;
- whether optional source datasets/tool outputs are separate downloads.

The complete Gaia DR3 archive is never implied by the v1 install size.

## Graphics correctness versus performance

A platform can pass correctness and still miss the performance tier. Report them separately:

- **correctness:** math tests, seams, validation layers, finite HDR, astronomy tolerance, deterministic tier;
- **performance:** frame-time distribution, memory peaks, streaming latency, shader/pipeline warm-up;
- **packaging:** clean install, data discovery, permissions, signing/notarization where applicable;
- **UX:** controls, display scaling, input, and error messages.

Do not lower correctness tolerances to label a slow platform “supported.” It may be supported at a lower named quality preset if the product contract is still met.

## Driver and regression policy

- Every result records the exact driver/runtime version.
- A supported profile is a tested range/list, not “latest” forever.
- New drivers are smoke-tested before replacing a release baseline where possible.
- Vendor workarounds are isolated, documented, and covered by a device test.
- Device loss, missing capability, incompatible cache, and insufficient memory produce actionable errors rather than crashes.
- Pipeline caches are device/driver scoped and invalidated according to Vulkan identifiers.

## CI policy

Milestone 0 should establish at minimum:

- formatting/static checks and unit tests on every primary change;
- one reference-platform CPU/build job;
- shader compilation/reflection/ABI tests;
- a swapchain-free Vulkan smoke job when suitable GPU infrastructure exists;
- compile or smoke jobs for experimental platforms as resources allow;
- scheduled/manual full route runs on named physical devices, since generic hosted CI does not prove GPU support.

No CI badge should imply runtime GPU coverage it does not actually execute.

## Promotion checklist

A profile moves from experimental to supported only when:

1. clean build/install instructions are reproduced by someone or automation outside the original development tree;
2. the capability report satisfies the declared profile;
3. all mandatory validation tests pass;
4. cold and warm benchmark routes meet the performance/memory thresholds;
5. no undisclosed renderer or data-quality fallback is active;
6. known limitations are published; and
7. the release packages the exact dependencies, data manifests, and notices it needs.

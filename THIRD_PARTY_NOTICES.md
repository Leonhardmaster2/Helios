# Third-party notices

Status: **recorded for M0-adopted dependencies**. Full license texts ship with
each dependency source (vendored copies under `third_party/`, fetched copies
in the dependency cache). Pins and SHA-256: `cmake/deps.lock.json`.

| Component | Version / pin | License | Usage |
|---|---|---|---|
| SDL3 | release-3.4.14 | zlib | window/input/surface (external adapter for helios_vk) |
| Vulkan-Headers | vulkan-sdk-1.4.357.0 | Apache-2.0 | Vulkan API headers |
| volk | 2e19a77 (2026-08-14) | MIT | Vulkan loader function pointers |
| VulkanMemoryAllocator (VMA) | v3.2.1 | MIT | GPU memory allocation |
| Dear ImGui | 46d39d5 (2026-08-07) | MIT | debug UI (backend: SDL3 + Vulkan) |
| Tracy | v0.14.0 | BSD-3-Clause | profiler client (optional, HELIOS_ENABLE_TRACY) |
| doctest | v2.5.3 | MIT | unit test framework |
| nlohmann/json | v3.12.0 | MIT | JSON (config, reports, manifests) |
| SPIRV-Reflect | vulkan-sdk-1.4.357.0 | Apache-2.0 | shader reflection/ABI tests |
| Slang | v2026.14.1 | MIT | offline shader compiler (slangc → SPIR-V) |
| Diligent Engine | `aca22851` (`DiligentCore` `bb821b78`) | Apache-2.0; nested third-party components retain their own licences | statically linked Vulkan-only rendering backend and ImGui bridge |
| Mesa (lavapipe) | 26.2.0 | MIT | **experimental diagnostic only** (software Vulkan ICD, not shipped) |

Generated build artifacts (SPIR-V, manifests, embedded shader data) carry
their own generator/version stamps. No runtime data assets are shipped in M0.

## Published algorithms and fitted constants

No source files were copied for these; the implementations are Helios code
that reproduces published formulae or constants, credited where they are used.

| Item | Source | Used in |
|---|---|---|
| CIE 1931 colour-matching function fit (multi-lobe Gaussian) | Wyman, Sloan and Shirley, "Simple Analytic Approximations to the CIE XYZ Color Matching Functions", JCGT 2(2), 2013 | `src/blackhole/blackbody.cpp` |
| Thin-disk flux for `a* = 0` | Page and Thorne, ApJ 191, 499 (1974) | `src/blackhole/thin_disk.cpp`, `shaders/black_hole.slang` |
| ACES RRT+ODT fitted curve and matrices | Stephen Hill's fit, published in MJP's BakingLab (MIT) | `shaders/hdr_composite.slang`, `examples/black_hole_lab/main.cpp` |
| PCG3D integer hash | Jarzynski and Olano, "Hash Functions for GPU Rendering", JCGT 9(3), 2020 | `shaders/lib/sky.slang` |
| HLLC approximate Riemann solver; exact Riemann solution used for validation | Toro, "Riemann Solvers and Numerical Methods for Fluid Dynamics", 3rd ed., Springer 2009 | `shaders/hydro_advance.slang`, `examples/deep_sky_lab/gas_validation.cpp` |
| Octahedral mapping of directions | Cigolle et al., "A Survey of Efficient Representations for Independent Unit Vectors", JCGT 3(2), 2014 | `shaders/lib/hydro_radiation.slang` |
| Particle-mesh gravity with isolated boundaries | Hockney and Eastwood, "Computer Simulation Using Particles", 1988 | `examples/deep_sky_lab/galaxy_simulation.cpp` |
| 13-tap downsample / 9-tap tent upsample glare pyramid | Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare", SIGGRAPH 2014 | `shaders/hdr_glare.slang` |

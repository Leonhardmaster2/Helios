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
| Mesa (lavapipe) | 26.2.0 | MIT | **experimental diagnostic only** (software Vulkan ICD, not shipped) |

Generated build artifacts (SPIR-V, manifests, embedded shader data) carry
their own generator/version stamps. No runtime data assets are shipped in M0.

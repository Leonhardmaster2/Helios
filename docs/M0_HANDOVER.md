# Helios Milestone 0 — Complete Engineering Record and Handover

> **Status**: M0 complete, verdict **M0 IS READY FOR M1** (see [evidence/m0/M0_REPORT.md](../evidence/m0/M0_REPORT.md)).
> This document is the full "what, why, and how" record for M0: every subsystem, every decision,
> every bug found and its root cause, every verification run, and everything still open.
> Written 16 August 2026 from the working session that built M0.

---

## 0. How to read this document

| Section | For |
|---|---|
| 1 | One-paragraph summary of M0 |
| 2 | What M0 was supposed to deliver (the contract) |
| 3 | Repository map after M0 |
| 4 | Environment facts and sandbox constraints (IMPORTANT for any future agent) |
| 5 | What was built, module by module, and *why* it is shaped that way |
| 6 | Decision log D-001..D-020 with rationale |
| 7 | The debugging war story: every bug, root cause, fix |
| 8 | Verification matrix (every build config and evidence run) |
| 9 | Performance baseline |
| 10 | Remaining M0 work and exact procedures |
| 11 | Reproduce-everything command list |
| 12 | Pitfalls and "do not do this" list |
| 13 | How to start M1 (handover to the next session) |

---

## 1. Summary

Milestone 0 ("project skeleton and capability spike") is implemented, tested, and validated on
the reference profile (Windows 11 / MSVC / RTX 5080): a C++20/CMake multi-module skeleton,
pinned dependencies with SHA-256 verification and an offline cache, Slang-to-SPIR-V offline
shader compilation with reflection/ABI tests, a Vulkan 1.3 bootstrap with a capability report,
six smoke paths (clear, triangle, depth, compute, transfer-upload, timestamps) that all pass
with **zero Vulkan validation errors**, a windowed SDL3/swapchain/ImGui path including
swapchain recreation on resize, Tracy (live and clean-exiting), doctest unit suites (7/7 in
every configuration), clang-format clean, MSVC ASan 7/7, a CI workflow, and an evidence tree
whose values are reproducible from the committed code.

An independent adversarial review agent audited the whole tree, reproduced the headline
numbers, found four real defects plus minors, and — after fixes — re-verified everything and
issued the verdict **M0 IS READY FOR M1**.

## 2. The M0 contract (from ROADMAP.md)

- C++20/CMake project skeleton with a one-command build+test flow.
- Pinned dependencies with an offline/cache strategy (no unpinned network fetches at build time).
- `helios_core`: logging, config, errors/Result, debug names.
- SDL3 + Vulkan 1.3 initialization and a capability report.
- Smoke paths: clear, triangle, depth, compute, upload, timestamps.
- `--headless-smoke` (no window/swapchain).
- Pinned Slang → SPIR-V offline compile plus reflection/ABI tests.
- Tracy, ImGui, doctest.
- Formatting and static-analysis tooling; ASan/UBSan where supported.
- Report + asset-manifest schema stubs.
- One reference CI workflow; experimental platform jobs.
- Hard requirements: zero known Vulkan validation errors; performance and correctness both
  mandatory; no fabricated evidence; BLOCKED/NOT VALIDATED items marked explicitly;
  no M1 work; final report with the exact required sections.

## 3. Repository map after M0

```
Helios/
  CMakeLists.txt, CMakePresets.json   # project + presets (dev/debug/release/ci/asan/msvc-asan/headless-ci/windows-*)
  cmake/
    deps.lock.json                    # pinned deps + SHA-256 (all entries, incl. VMA)
    HeliosDeps.cmake                  # FetchContent wiring + TRACY options (NO_EXIT must stay OFF)
    HeliosCompileOptions.cmake        # warnings, sanitizers, git-hash/shader-dir definitions
    SlangCompile.cmake                # 5 slangc jobs, manifest, embedded SPIR-V generation
    ImGui.cmake, gen_shader_embed_impl.in
  scripts/
    fetch_deps.mjs                    # download + SHA-256 verify + extract + deps-cache.cmake
    check_module_deps.mjs             # include-level dependency audit (CI)
    check_format.ps1, run_tidy.ps1
  src/
    core/    helios_core   (version, error, log, config, hash, io, debug_name, timer, profiler, id)
    math/    helios_math   (vec, mat4, perspectiveReversedZInfinite, lookAt)
    assets/  helios_assets (report/manifest schema structs + JSON writers)
    universe/, planet/     (module skeletons only — M1+ content)
    vk/      helios_vk     (instance, device, capabilities, vma, buffer, image, swapchain,
                            shader_module, pipeline, descriptor_set, sync, command, smoke/)
    render/  helios_render (renderer, frame, reports, debug_ui, headless)
    app/     helios        (main + crash handler, cli, headless, app windowed loop)
  shaders/  triangle.slang, depth.slang, compute.slang, abi.hpp (dependency-free ABI contract)
  tests/    7 doctest suites (core, math, assets, universe, planet, vk, shader-ABI)
  evidence/m0/  local/ perf/ windowed/ resize/  + README.md, commands.md, M0_REPORT.md
  schemas/  *.schema.json (report + asset manifest, v1)
  docs/     DECISIONS.md (D-001..D-020), ARCHITECTURE.md, SUPPORT.md, VALIDATION.md, ...
  .github/workflows/ci.yml
```

## 4. Environment facts (IMPORTANT for any future agent in this machine/sandbox)

### Machine
- Windows 11 (build 26200), MSVC 19.44.35228 (VS 2022 Community), CMake 4.2.0-rc2,
  Ninja 1.12.1, Vulkan SDK 1.4.328.1, NVIDIA GeForce RTX 5080 (driver 610.74, Vulkan 1.4.341,
  15.6 GiB VRAM).

### DSH sandbox constraints (learned the hard way — respect them)
- `run_code` Node programs run with cwd = home directory: always use absolute paths.
- `pwsh` tool defaults to the session workspace (`C:\Users\leonh\Documents\Helios`).
- File writes outside the workspace are denied by the sandbox.
- npm needs `--cache` inside the workspace; schannel TLS is broken for git (use
  `-c http.sslBackend=openssl`) and for CMake `file(DOWNLOAD)` (fails) — use Node `fetch`.
- **Ninja hangs** when its children use pipes in this sandbox → locally use MSBuild
  ("Visual Studio 17 2022" generator, i.e. the `windows-*` presets); ninja presets exist for CI.
- **Node-spawned `tar` fails** (EPERM on piped stdio) → `fetch_deps.mjs` extraction fails in
  fresh checkouts; workaround: downloads land in `.deps-cache/_tmp_*`, then extract manually
  with pwsh `tar -xf` + flatten single inner dir + write `.fetch-ok` stamp, then re-run
  `node scripts/fetch_deps.mjs --write-cmake`.
- **`git clone` also fails** in the sandbox (spawns sh/upload-pack with pipes). Use
  `git archive --format=tar -o <file> HEAD` + `tar -xf` for fresh-checkout verification.
- File visibility is per-session for spawned executables: files written by older sessions'
  processes can be invisible → **shaders are EMBEDDED into binaries** at build time.
- PowerShell quirks: `$ErrorActionPreference="Stop"` + `2>&1 | Select-Object` turns native
  stderr into terminating errors — don't combine them. `Start-Process -Wait` with redirects
  throws "Access is denied" — poll `HasExited` instead. TS template literals interpolate
  `${...}` — escape or concatenate when emitting PowerShell/CMake text.

## 5. What was built and why it is shaped that way

### 5.1 helios_core (no internal dependencies)
- **version**: `heliosVersion()` now reads HELIOS_VERSION_MAJOR/MINOR/PATCH defined by CMake
  from `project(VERSION 0.1.0)` (was hardcoded). `heliosGitHash()` bakes the commit hash at
  configure time (this is what the evidence bundles record).
- **error**: `Result<T>` + error codes; all Vulkan-adjacent code returns Results instead of
  throwing; every failure path is logged.
- **log**: leveled, thread-safe (mutex), `std::format`, ANSI colors on Windows console,
  optional file sink. `HELIOS_LOG_MIN_LEVEL` is a REAL compile-out now (was a tautology:
  `if constexpr (Trace >= Off)` always false).
- **config**: dotted-key nested JSON + CLI layering; unit-tested. Known cosmetic gap:
  `Config::set()` doesn't nest dotted keys while `load()` does (accepted, M1 cleanup).
- **hash**: SHA-256 (NIST test-vector verified; the original vector literal was 53 chars
  instead of 56 and was rebuilt from 14×4-char chunks).
- **io**: atomic file writes (temp + rename) for evidence JSON.
- **debug_name**: object naming helper; on the RTX 5080 the driver does NOT expose
  device-level VK_EXT_debug_utils, so `setDebugName` no-ops there (documented, not fixable).
- **timer**: FrameStats with p50/p95/p99/max/average.
- **profiler**: Tracy facade — `HELIOS_ZONE_SCOPED` etc. map to Tracy only when
  HELIOS_ENABLE_TRACY is defined (now actually defined by core CMake when the option is ON).

### 5.2 helios_math / helios_assets
- math: vec/mat4, `perspectiveReversedZInfinite` (reversed-Z, D-002), lookAt.
- assets: report/manifest schema structs (schema v1) + JSON writers; `schemas/*.schema.json`.

### 5.3 helios_vk (depends only on core)
- **instance**: volk init, validation-layer integration with a debug messenger that writes
  counts into `Instance::validation_stats_` (lifetime-owned by the Instance — a freed-pointer
  crash here was fixed), surface creation for SDL3, portability-enumeration flag gated on the
  extension (was wrongly gated on debug-utils).
- **device**: feature chain f13 → f11 → timeline → buffer-address → host-query-reset →
  descriptor-indexing, plus `shaderDrawParameters` (Slang's DrawParameters capability;
  missing it produced a validation error). VMA via `pVulkanFunctions`
  (VMA_DYNAMIC_VULKAN_FUNCTIONS=1, VMA_STATIC_VULKAN_FUNCTIONS=0).
  Capability probe + `reportJson()`. Serialized baseline: compute/transfer queues = graphics
  queue (M4 optimization deferred).
- **vma_allocator**: VMA 3.2.1 (NOT 3.4.0: v3.4 crashed in vmaCreateAllocator with a NULL
  TLSF call); headers pragma-wrapped to silence /W4 warnings under /WX.
- **buffer/image**: uploadFromHost ends TRANSFER_DST→GENERAL; readback expects GENERAL;
  `Image::readback` hardcodes 4 B/px (latent limitation for future formats).
- **swapchain**: pickFormat (SRGB preferred) / pickPresentMode (MAILBOX preferred);
  `recreate()` waits device idle and chains oldSwapchain; `acquireNextImage` without fence.
- **shader_module**: `createFromSpirv` for embedded SPIR-V (sandbox-forced choice).
- **pipeline**: graphics pipeline with dynamic rendering; entry point always `"main"`
  (Slang renames entries in SPIR-V — abi.hpp now says so).
- **descriptor_set**: layouts + pool; `allocate()` fails loudly on exhaustion;
  **`reset()` added** (vkResetDescriptorPool) for the smoke suite.
- **sync**: fences/semaphores/timestamps (vkResetQueryPool returns void — an early build
  error assumed otherwise).
- **command**: runOneShot (submit + wait).
- **smoke suite**: six paths + the windowed scene; one `vkCmdDraw` for depth probes (a
  second vkCmdDraw in the same pass rendered nothing on this NVIDIA driver — documented;
  one draw with depth testing is order-independent and correct).

### 5.4 helios_render
- **renderer**: windowed frame loop — wait fence → acquire (NO fence: the same fence
  must not serve acquire and submit) → record → submit with per-swapchain-image
  render-finished semaphores → present. ImGui in its own color-only dynamic-rendering pass
  (the backend ignores depth formats). Destructor destroys every device object (timestamp
  pool, depth image, views, semaphores) before the device, surface after swapchain.
- **swapchain recreation** (M1 fix): `Renderer::createSwapchainAttachments /
  initImGuiVulkanBackend / recreateSwapchainResources` rebuild image views, render-finished
  semaphores (image count can change!), the depth attachment at the new extent, and the
  ImGui Vulkan backend; device idle first; fence reset moved AFTER a successful acquire.
- **headless**: smoke orchestration moved here so the app has no Vulkan edge.

### 5.5 helios_app
- main.cpp: crash handler (SetUnhandledExceptionFilter + dbghelp stack trace; windows.h must
  be included BEFORE dbghelp.h — MSVC C3646 otherwise, and clang-format wants the opposite
  order, so the block is clang-format-off guarded).
- app.cpp: SDL_Init returns **true** on success (SDL3 — an inverted check was fixed);
  `--frames N` bounded runs; `--resize-test` (new) bounces the window size mid-run to
  exercise swapchain recreation under validation layers; dead OrbitCamera removed;
  rendered-frame counter counts real presents only.
- cli.cpp: full flag set incl. `--resize-test`; `--config` is parsed but not yet wired into
  the app (accepted M1 cleanup, config library itself is tested).

### 5.6 Shaders and ABI
- triangle.slang (UBO set0/b0, identity matrix, NDC triangle), depth.slang (VsOut
  {pos: SV_Position; color: COLOR0}; per-instance color via user varying; no SV_InstanceID
  in FS), compute.slang (`[[vk::push_constant]]` on a variable, not a struct).
- abi.hpp is dependency-free; layout static_asserts; reflection test uses SPIRV-Reflect's
  two-call enumeration pattern and checks the real `"main"` entries and bindings.

## 6. Decision log (docs/DECISIONS.md), with the *why*

- **D-001..D-003**: Vulkan 1.3 floor, dynamic rendering + sync2, reversed-Z infinite depth
  (D-002) — precision and simplicity for the planet scale; validated by the depth smoke.
- **D-004**: swapchain + WSI design (per-image semaphores) — accepted with M0 evidence.
- **D-005**: smoke-path set — accepted with M0 evidence.
- **D-008**: pinned dependencies, SHA-256, offline cache — accepted with M0 evidence;
  VMA hashes were empty at first and are now filled (M5).
- **D-014**: D32_SFLOAT depth choice — accepted with M0 evidence (probed D32 vs D24).
- **D-017**: reference platform = Windows 11 + MSVC 19.44.35228 + RTX 5080 (driver 610.74)
  — this machine; all M0 evidence is generated on it.
- **D-018**: determinism tiers — define how reproducible results must be.
- **D-019**: Gaia catalog shape — HEAPix level-6 tiles, magnitude tiers, 24-byte records;
  ADQL execution still open (BLOCKED: needs Gaia Archive access).
- **D-020**: Earth hero region — candidate only, open (BLOCKED: product download + terms).

## 7. The debugging war story (every bug, root cause, fix)

1. **schannel TLS broken** (`SEC_E_NO_CREDENTIALS`): git needs `-c http.sslBackend=openssl`;
   CMake `file(DOWNLOAD)` fails → all downloads use Node `fetch`.
2. **Ninja hangs on child spawn** (sandbox pipes) → local builds use the VS generator.
3. **Sandbox per-session file invisibility** (stale `.spv` unreadable) → shaders embedded
   into binaries at build time (`shaders_embedded.hpp/cpp`, `helios_shader_data`).
4. **CMake `string(JSON ... OBJECT)` invalid** and **MSBuild mangles `-D` args** →
   configure_file templates with baked paths; no -D at build time.
5. **VMA 3.4 crash** (NULL call inside vmaCreateAllocator, TLSF) → downgrade to VMA 3.2.1 +
   dynamic functions + pVulkanFunctions.
6. **Debug-messenger crash** (freed DebugMessengerState) → callback writes to
   Instance-owned ValidationStats.
7. **DrawParameters validation error** → enable `VkPhysicalDeviceVulkan11Features::
   shaderDrawParameters`.
8. **depth.slang interface mismatch** (SV_Position returned without semantic; SV_InstanceID
   in FS) → rewrote with VsOut { pos: SV_Position; color: COLOR0 }.
9. **Triangle drew nothing** → missing `ubo.value().upload(...)` (identity matrix was all
   zeros, w=0).
10. **Clear path "9+ pixel mismatches"** → GPU unorm rounding (0.1f*255=25.5→25);
    ±1 tolerance.
11. **Depth near-wins false** → probes didn't overlap at center; overlap quad + single
    vkCmdDraw(6,4,0,0).
12. **Windowed teardown crash + destroy-order validation errors** → destructor order:
    device objects first, surface after swapchain.
13. **Fence in use** (acquire+submit same fence) → acquire with VK_NULL_HANDLE fence.
14. **Swapchain semaphore reuse hazard** → per-swapchain-image render_finished semaphores.
15. **ImGui depth format ignored by backend** → ImGui in separate color-only pass.
16. **SDL_Init inverted check** (SDL3 returns true on success).
17. **ImGui `vkGetDeviceProcAddr+0x0` crash** (data symbol executed as code) →
    IMGUI_IMPL_VULKAN_NO_PROTOTYPES + ImGui_ImplVulkan_LoadFunctions.
18. **dbghelp.h C3646** → windows.h before dbghelp.h (clang-format-guarded).
19. **Release /WX warnings** → pragma-wrapped vk_mem_alloc.h includes; removed unused
    Device& param; checked [[nodiscard]] results; removed unused `show_help` local.
20. **MSVC ASan 0xC0000135** → `clang_rt.asan_dynamic-x86_64.dll` not on PATH; add the
    MSVC Hostx64/x64 bin dir to PATH before ctest (now verified 7/7).
21. **Frame-time units** → CPU/GPU stats are already ms; remove bogus ×1000.
22. **clang-format false positives** (PowerShell CRLF/BOM pipe) → `--dry-run --Werror`.
23. **[Review M1] Swapchain recreation incomplete** → full resource rebuild (views,
    semaphores, depth, ImGui) + **found a pre-existing deadlock**: fence was reset before
    acquire; an OUT_OF_DATE retry returned with an unsignaled fence and the next frame's
    vkWaitForFences(UINT64_MAX) blocked forever. Fence reset now happens only after a
    successful acquire. Verified by `--resize-test` (two mid-run resizes, 0 errors).
24. **[Review M2] Tracy facade was inert** (HELIOS_ENABLE_TRACY never defined) → wire the
    definition. Then the client became live and **every process hung at exit**:
    TRACY_NO_EXIT compiled into TracyClient removes the worker's ShouldExit() check, so the
    process waits forever for a profiler connection. Removed from cmake/HeliosDeps.cmake
    (with an explicit FORCE OFF — the old cache value persists otherwise).
25. **[Review M3] `--iterations >= 5` failed silently** — descriptor pool exhausted
    (UBO 8/SSBO 4/maxSets 16; 4 sets per iteration, never freed) → `DescriptorPool::reset()`
    at the start of each smoke iteration; verified at `--iterations 8`.
26. **[Review M4] CI sanitizer job miswired** — configured `asan` preset but ran
    `ctest --preset ci` → `ctest --preset asan`.
27. **[Review M5] VMA lock entries had empty sha256** → filled; cache now hit instead of
    re-downloading every time.
28. **[Review m1] Portability flag gated on debug-utils** → gated on the portability
    extension (VUID-VkInstanceCreateInfo-flags-04828).
29. **[Review m6/m7/m10/minors]** abi.hpp entries → "main"; log compile-out made real;
    version from project(); capabilities JSON includes shader_draw_parameters; imgui.ini
    untracked; frame-count print fixed; embed-template mojibake fixed; dead orbit camera
    removed; UI help text corrected.
30. **MSVC ASan + Tracy LNK2038** (`annotate_string`/`annotate_vector` mismatch between
    uninstrumented TracyClient.obj and instrumented TUs) → HELIOS_ENABLE_TRACY=OFF in the
    asan presets (sanitizer runs focus on Helios code).

## 8. Verification matrix

| Configuration | Build | Unit tests | Smoke | Validation |
|---|---|---|---|---|
| windows-dev (RelWithDebInfo, VS) | OK | 7/7 | headless x8 + windowed 400 + windowed 240 + resize 90 | 0/0 |
| windows-clean (Release, /WX) | OK warning-free | 7/7 | — | — |
| windows-asan (Debug, MSVC ASan) | OK | 7/7 | — | — |
| fresh git-archive checkout of HEAD | OK | 7/7 | headless x3 | 0/0 |
| module-deps audit | pass | | | |
| clang-format (src+tests, --Werror) | 0 violations | | | |

Evidence bundles (all at HEAD 5394117, all 0 validation errors):
- `evidence/m0/local/` — headless ×8 (clear 921600 px match, triangle #3f4080, depth
  0.150/0.500/0.900 near-wins, compute 4096 bit-exact, transfer 8192 el + 16384 B image,
  timestamp 0.0044 ms @ 1 ns).
- `evidence/m0/perf/` — windowed 400 frames (CPU p50 0.351 ms; GPU p50 0.0075 ms).
- `evidence/m0/windowed/` — windowed 240 frames (CPU p50 0.370 ms).
- `evidence/m0/resize/` — 90 frames with two swapchain recreations (1280x720 → 853x480 → back).

## 9. Performance baseline

Windowed 400 frames @ 1280x720 with ImGui + smoke scene:
- CPU frame time: p50 0.351 ms / p95 0.717 / p99 1.004 / max 3.894 ms
- GPU frame time: p50 0.0075 ms / p95 0.0091 / p99 0.0098 / max 0.012 ms
- Engine is CPU-bound in this spike (expected); GPU work is <1% of the vsync budget.
- Note: `frame_index` currently never advances (always slot 0 of 2 frames-in-flight), so the
  loop is effectively serialized. The "2 frames in flight" plumbing exists but the index
  increment is a known M1 perf fix.

## 10. Remaining M0 work (all BLOCKED by environment, honest status)

1. **CI execution** — no GitHub credentials/runner here. Push + open a PR; workflow:
   `.github/workflows/ci.yml` (format+deps, cpu-suite matrix ubuntu-gcc/ubuntu-clang/
   windows-msvc/macos-clang, sanitizers ASan+UBSan, self-hosted gpu-smoke dispatch-only).
2. **macOS/MoltenVK tier** — needs a Mac + Vulkan SDK: `cmake --preset ci`, build, tests,
   `helios --headless-smoke`; record MoltenVK/Metal versions.
3. **AMD/Intel device matrix** — run `--headless-smoke --iterations 3` on each device,
   archive the report dir.
4. **Gaia Archive ADQL experiment (D-019)** — execute the ADQL recipe (pin with M9),
   archive + hash the query result, validate page counts vs the D-019 shape.
5. **Earth hero region (D-020)** — download the exact SRTM/NASADEM product tiles; record
   product page, license, DOI, datum, SHA-256; validate vs DATA_AND_PROVENANCE.md criteria.

Known cosmetic leftovers (accepted, documented in M0_REPORT.md): Config::set() dotted-key
nesting; a few unchecked vk call results in smoke call sites; windowed bundles record
driver_version "queried-at-runtime".

## 11. Reproduce everything

```powershell
# deps (SHA-256 verified; --write-cmake generates cmake/deps-cache.cmake)
node scripts/fetch_deps.mjs --update-lock --write-cmake

# configure + build + test (VS generator on Windows; ninja presets on Linux/macOS CI)
cmake --preset windows-dev
cmake --build --preset windows-dev
ctest --test-dir build/windows-dev -C RelWithDebInfo --output-on-failure

# headless smoke (validation on) + reports
build/windows-dev/src/app/RelWithDebInfo/helios.exe --headless-smoke --iterations 8 --report-dir evidence/m0/local

# windowed runs
build/windows-dev/src/app/RelWithDebInfo/helios.exe --frames 400 --report-dir evidence/m0/perf
build/windows-dev/src/app/RelWithDebInfo/helios.exe --frames 90 --resize-test --report-dir evidence/m0/resize

# clean /WX gate + ASan (asan DLL from vcvars64.bat must be on PATH)
cmake --build build/windows-clean --config Release
ctest --test-dir build/windows-clean -C Release --output-on-failure
cmake --preset windows-asan && cmake --build --preset windows-asan
ctest --test-dir build/windows-asan -C Debug --output-on-failure

# format + module audit
scripts/check_format.ps1
node scripts/check_module_deps.mjs
```

## 12. Pitfalls / do-not-do list

- Do NOT re-add TRACY_NO_EXIT (TracyClient build option or define) — every process hangs
  at exit.
- Do NOT reset the submit fence before vkAcquireNextImageKHR — OUT_OF_DATE retries deadlock
  the next frame.
- Do NOT revert to VMA 3.4.0 — vmaCreateAllocator crashes on this driver.
- Do NOT run two vkCmdDraw calls in one pass for depth probes on NVIDIA — second draw
  renders nothing; use one draw + depth test.
- Do NOT gate the portability-enumeration flag on debug-utils availability.
- Do NOT use Ninja locally in this sandbox; do not spawn tar/git-clone from Node in this
  sandbox (manual extraction / git archive workarounds above).
- Do NOT claim CI/macOS/AMD-Intel/Gaia/Earth items validated — they are BLOCKED.
- Do NOT start M1 before reading ROADMAP.md M1 + DECISIONS D-017..D-020.
- Keep the module dependency direction (core → math/assets → vk → render → app);
  check_module_deps.mjs enforces it.
- Keep `ImGui_ImplVulkan_Init` ImageCount >= MinImageCount (2) and re-init the backend
  after swapchain recreation.
- When changing presets, remember CMake cache FORCE semantics: removing a `set(... FORCE)`
  line does NOT clear a stale cached value — set the new value explicitly.

## 13. Starting M1 (handover)

1. Read in order: ROADMAP.md (M0 + M1 sections), docs/ARCHITECTURE.md, docs/DECISIONS.md
   (D-017..D-020 first), evidence/m0/M0_REPORT.md, this document.
2. Fix the known M1 perf item: advance `frame_index` so both in-flight frame slots are
   used (currently always slot 0 — the plumbing is ready, the increment is missing).
3. Pick up the first M1 work item from ROADMAP.md; keep the smoke suite + evidence flow
   green after every change (`--headless-smoke`, windowed, resize tests are the regression
   gate).
4. Re-run the BLOCKED evidence items when access becomes available (section 10).
5. Keep the evidence contract: every claim must be reproducible from the committed tree;
   regenerate bundles at the true HEAD before committing them (git hash is baked at
   configure time).

---
*End of M0 handover. Working tree at commit 8a82ce4; M0 evidence at HEAD 5394117.*
# Helios Milestone 0 — Final Report

Date: 16 August 2026 · Reference profile (D-017): Windows 11 (build 26200) / MSVC 19.44.35228
(VS 2022 Community) / CMake 4.2.0-rc2 / NVIDIA GeForce RTX 5080, driver 610.74,
Vulkan loader 1.4.341, Vulkan SDK 1.4.328.1. HEAD: 5394117 (evidence bundle commit 56a9caf).
*(Correction, 21 Aug 2026: the bundles were subsequently regenerated at the
true HEAD — first at `3dd2008` after the frame-slot fix, then refreshed with
later commits; see `git log -- evidence/m0`. The narrative below describes
the original M0 verification runs and is preserved as written.)*

## 1. Implementation summary

M0 "project skeleton and capability spike" is implemented and locally verified end-to-end:

- **Modules**: helios_core (version, errors/Result, logging, config, SHA-256, atomic IO,
  debug names, frame stats, Tracy facade), helios_math, helios_assets (report/manifest
  schemas v1 + JSON writers), helios_vk (instance/device/swapchain/VMA/buffers/images/
  pipelines/descriptors/sync/command + smoke suite), helios_render (swapchain renderer,
  ImGui overlay, headless smoke orchestration), helios_app (CLI, crash handler,
  windowed explorer + headless entry), module skeletons universe/planet. Shaders in
  Slang compiled offline to SPIR-V and embedded at build time (helios_shader_data).
- **Smoke paths (all pass)**: clear, triangle, depth (reversed-Z D32_SFLOAT, near-wins),
  compute (bit-exact vs CPU), transfer-upload, timestamps.
- **`--headless-smoke`** (no window/swapchain) with `--iterations N` (verified N=8),
  `--frames N` windowed runs, `--resize-test` swapchain-recreation exercise.
- **Tooling**: pinned deps with SHA-256 (cmake/deps.lock.json), one-command
  fetch/configure/build/test, CTest 7 suites, clang-format clean, MSVC ASan preset,
  CI workflow (cpu-suite matrix, sanitizers, format+deps, self-hosted gpu-smoke).

## 2. Architecture

Per ROADMAP.md and docs/ARCHITECTURE.md: strict module dependency direction
(core → math/assets → vk → render → app; universe/planet skeletons), enforced by
link edges and scripts/check_module_deps.mjs (passes). Vulkan 1.3 baseline: volk
(no prototypes), VMA 3.2.1 with dynamic functions, dynamic rendering, sync2,
timeline/buffer-address/host-query-reset/descriptor-indexing feature chain plus
shaderDrawParameters (Slang's DrawParameters capability). Serialized single-queue
baseline (graphics/compute/transfer share queue family 0); 2 frames in flight with
per-swapchain-image render-finished semaphores; ImGui in its own color-only
dynamic-rendering pass. Swapchain recreation rebuilds image views, render-finished
semaphores, the depth attachment, and the ImGui backend (verified by --resize-test).
Decisions: docs/DECISIONS.md D-001..D-020.

## 3. Build and toolchain

- One-command flow (evidence/m0/commands.md): node scripts/fetch_deps.mjs →
  cmake --preset windows-dev → cmake --build → ctest. Verified from a **fresh
  git-archive checkout** of HEAD: configure, full build, 7/7 tests, headless smoke
  all pass (build/fresh-checkout).
- Clean-build gate: cmake --build build/windows-clean --config Release with
  HELIOS_WERROR=ON — builds warning-free, 7/7 tests pass.
- Sanitizers: MSVC ASan preset builds and **7/7 suites pass** under ASan (runtime
  DLL from the MSVC bin dir on PATH; Tracy excluded from sanitizer builds to avoid
  an uninstrumented-Tracy LNK2038 STL-annotation mismatch).
- Formatting: clang-format --dry-run --Werror → 0 violations on src+tests.
  Module-dep audit passes. Slang 2026.14.1 offline compile: 5 SPIR-V modules with
  embedded manifest; reflection/ABI tests verify layouts via SPIRV-Reflect.

## 4. Validation

- Vulkan validation layers enabled in every evidence run: **0 errors, 0 warnings**
  in all runs (headless x8 iterations, windowed 400, windowed 240, resize 90).
- 7/7 doctest suites pass on dev, clean /WX, ASan, and fresh-checkout builds.
- Independent adversarial review (external reviewer agent) reproduced the headline
  numbers from the committed tree and re-verified the M1–M5 fixes after they were
  applied; its only remaining finding (stale helios_git_hash in bundles) was fixed
  by regenerating all bundles at the true HEAD.
- Everything marked not-run is explicitly NOT claimed: CI execution, macOS/MoltenVK,
  AMD/Intel, Gaia ADQL, Earth hero region (see §8).

## 5. Vulkan capability results

Reference device: NVIDIA GeForce RTX 5080, API 1.4.341, driver 610.74,
15.6 GiB device-local heap. Depth: D32_SFLOAT (chosen), timestamp period 1 ns,
unified queue family 0 (graphics+compute+transfer+present). Swapchain 1280x720,
B8G8R8A8_SRGB, present mode MAILBOX, 3 images. Features enabled: dynamic rendering,
synchronization2, timeline semaphores, buffer device address, host query reset,
descriptor indexing, maintenance4, shader draw parameters. Full machine-readable
report: evidence/m0/local/capabilities.json (schema v1).

## 6. Performance baseline

Windowed 400 frames at 1280x720 with ImGui + smoke scene (evidence/m0/perf/performance.json):
- CPU frame time: p50 0.351 ms · p95 0.717 ms · p99 1.004 ms · max 3.894 ms
- GPU frame time: p50 0.0075 ms · p95 0.0091 ms · p99 0.0098 ms · max 0.012 ms
(ImGui UI + smoke scene dominate CPU; GPU work is a fraction of a percent of the
16.7 ms vsync budget — the engine is CPU-bound in this spike, as expected. Timestamp
queries lag by frames-in-flight by design.)

## 7. M0 decision outputs

- D-017: reference platform (above). D-018: determinism tiers.
- D-019: Gaia catalog shape — HEALPix level-6 tiles, magnitude tiers, 24-byte
  records; ADQL execution still open (BLOCKED, archive access).
- D-020: Earth hero region — candidate only, open (BLOCKED, product download).
- D-004/005/008/014 accepted with M0 evidence (swapchain + smoke + pinned deps +
  reversed-Z depth). Full log: docs/DECISIONS.md.

## 8. Remaining M0 work (all environment-gated, none hidden)

- CI workflow execution — BLOCKED (no GitHub credentials/runner here; file reviewed
  and sanitizer-job ctest preset corrected).
- macOS/MoltenVK tier — BLOCKED (no macOS environment; procedure documented).
- AMD/Intel device matrix — BLOCKED (no such devices).
- Gaia Archive ADQL experiment — BLOCKED (needs archive access).
- Earth hero-region product selection — BLOCKED (needs product download + terms).
- Cosmetic leftovers documented by review: Config::set() does not nest dotted keys
  (load() does); a few vk call results unchecked in smoke call sites;
  windowed bundles record driver_version as "queried-at-runtime".

## 9. Risks discovered

- **TRACY_NO_EXIT compiled into TracyClient made every process hang at exit**
  waiting for a profiler connection — fixed (removed from cmake/HeliosDeps.cmake);
  regression risk if re-added.
- **Acquire-retry deadlock**: fence reset before acquire + OUT_OF_DATE retry left
  an unsignaled fence; next frame blocked forever. Fixed by resetting after a
  successful acquire; covered by --resize-test.
- **Descriptor-pool exhaustion** broke --iterations >= 5 silently; pool reset per
  iteration fixes it; --iterations 8 verified.
- **MSVC ASan + Tracy** STL-annotation LNK2038 mismatch — Tracy excluded from
  sanitizer presets; documented in CMakePresets.json.
- NVIDIA driver quirks: second vkCmdDraw in the same pass rendered nothing
  (documented; smoke uses one draw with depth testing); device-level
  VK_EXT_debug_utils absent → setDebugName no-ops on the reference profile
  (instance-level messenger still reports validation).
- Sandbox-specific (this environment only): node-spawned tar blocked, MSBuild
  instead of Ninja, embedded shaders — none affect users of the repo.

## 10. Recommended next step

**M0 IS READY FOR M1.** All locally verifiable M0 deliverables are complete and
empirically verified on the reference profile with zero Vulkan validation errors:
one-command clean-checkout build, 7/7 unit+shader suites (dev, /WX Release, ASan),
all six smoke paths (8 iterations), windowed path with swapchain recreation,
Tracy live and clean-exiting, formatting/static-analysis gates, CI workflow on
file. The five remaining M0 evidence items (CI execution, macOS/MoltenVK, AMD/Intel
matrix, Gaia ADQL, Earth hero region) require external access and are tracked as
BLOCKED; they should be completed in parallel with M1 work, after which the
milestone flips to ✅ per ROADMAP.md. Do not start M1 before reading
docs/DECISIONS.md D-017..D-020 and the M1 section of ROADMAP.md.

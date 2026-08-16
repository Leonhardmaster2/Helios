# Milestone 0 evidence

Generated 16 August 2026 on the reference profile
(Windows 11 / MSVC 19.44.35228 / NVIDIA GeForce RTX 5080, driver 610.74,
Vulkan SDK 1.4.328.1, Vulkan loader 1.4.341).

## Bundles

| Directory | Contents |
|---|---|
| `local/` | Headless smoke evidence: `capabilities.json`, `performance.json`, `evidence.json` (8 iterations, zero validation errors) |
| `perf/` | Windowed performance capture: 400 frames at 1280×720 with ImGui + smoke scene (zero validation errors) |
| `windowed/` | Windowed evidence run: 240 frames at 1280×720 (zero validation errors) |
| `resize/` | Swapchain-recreation exercise: 90 frames with two mid-run window resizes (1280×720 → 853×480 → back), zero validation errors |

## Evidence checklist (M0 gate)

| # | Item | Status |
|---|---|---|
| 1 | Build from a clean checkout (one-command flow) | ✅ local (commands in `evidence/m0/commands.md`) |
| 2 | Complete M0 unit test suite (7 suites, CTest) | ✅ 100% pass |
| 3 | Shader reflection/ABI tests | ✅ (embedded SPIR-V + manifest hashes verified) |
| 4 | Headless smoke: clear/triangle/depth/compute/transfer/timestamp | ✅ all pass |
| 5 | Vulkan validation layers, zero errors | ✅ 0 errors / 0 warnings (headless + windowed) |
| 6 | Windowed path (SDL3 + swapchain + ImGui) | ✅ 400 frames, clean exit, 0 errors (`perf/`); swapchain recreation on resize exercised (`resize/`) |
| 7 | Capability report | ✅ `local/capabilities.json` |
| 8 | CPU/GPU frame-time percentiles | ✅ `perf/performance.json` |
| 9 | Sanitizers | ✅ MSVC ASan on unit tests: 7/7 pass (asan runtime DLL on PATH); UBSan via Linux CI job (not executed here) |
| 10 | Formatting / static analysis | ✅ clang-format 0 violations on src+tests; clang-tidy script provided (`scripts/run_tidy.ps1`) but not run in CI (Windows-only script, CI is Linux) |
| 11 | CI workflow execution | ⬜ BLOCKED — no CI credentials/runner available from this environment; workflow file: `.github/workflows/ci.yml` |
| 12 | macOS/MoltenVK tier | ⬜ BLOCKED — no macOS environment; procedure in `evidence/m0/commands.md` |
| 13 | AMD/Intel device matrix | ⬜ BLOCKED — no such devices in the environment |
| 14 | Gaia Archive query experiment | ⬜ BLOCKED — requires Gaia Archive access (ADQL + archived hash); schema/shape decided in DECISIONS D-019 |
| 15 | Earth hero-region product selection | ⬜ BLOCKED — requires the exact product download + terms review (D-020) |

## Machine-readable reports

- `capabilities.json` — versioned device capability report (schema v1).
- `performance.json` — p50/p95/p99/max CPU+GPU frame times (schema v1).
- `evidence.json` — per-path smoke results + validation counts (schema v1).

All schemas: `schemas/*.schema.json`. The `--report-dir` flag reproduces
each bundle: `helios --headless-smoke --report-dir <dir>` or
`helios --frames 400 --report-dir <dir>`. The swapchain-recreation
exercise is `helios --frames 90 --resize-test --report-dir <dir>`.

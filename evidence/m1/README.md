# Milestone 1 evidence (partial — M1 IN PROGRESS)

Generated 21 August 2026 on the reference profile
(Windows 11 / MSVC 19.44 / NVIDIA GeForce RTX 5080). M1 is **not complete**:
this bundle covers the precision-laboratory slice only (VALIDATION.md
"Coordinate and precision suite" CPU portions + analytic depth checks).

## Bundle

| File | Contents |
|---|---|
| `precision.json` | Machine-readable M1 precision report (schema: `helios-m1-precision-report` v1): scenario, thresholds, measured values, per-check pass/fail, environment, device, validation counts |
| `capabilities.json` | Queried device capability report (schema v1, same as M0) |

## What was measured (all values computed at run time, none hard-coded)

- binary64 ULP spacing at 10⁷ / 10⁹ / 10¹² / 10¹³ m;
- rotating-frame phase observability: a 1 µs increment at 10⁷ days must
  remain visible (period-reduced phase computation);
- body-local transform round-trip error over 1000 trips (P-003 0.1 mm budget);
- root round-trip position/orientation error at surface, 10⁷, 10⁹, 10¹², 10¹³ m;
- root placement quantization error at 10¹³ m;
- camera reparenting: scripted route surface → orbit → 10⁹ → 10¹² → 10¹³ →
  surface with 5 hysteresis-band transitions, max pose/orientation jump per
  switch, and drift after 10001 repeated band crossings;
- screen-space jitter: stationary (must be exactly 0 px) and cross-storage
  path (barycentric vs body-frame evaluation) at each range;
- reversed-Z / infinite-far depth: analytic NDC z at 7 distances,
  monotonicity, near-plane and asymptote checks, plus the queried depth
  format (`D32_SFLOAT` on the reference device);
- scene-assembly boundary: near-record offset error at surface scale.

## Reproduce

```text
helios --precision-lab --report-dir evidence/m1/local
```

Exit code 0 = all thresholds pass; 3 = threshold failure (the report says
which). The route is fully deterministic (no RNG, fixed epoch).

## Thresholds (docs/VALIDATION.md)

- body-local transform error ≤ 1e-4 m (P-003 proposed 0.1 mm);
- reparenting discontinuity ≤ 1e-2 m per switch; drift ≤ 1e-2 m after 10k+
  transitions; orientation error ≤ 1e-9 rad;
- stationary jitter = 0 px (bitwise); cross-path jitter ≤ 0.5 px;
- depth: strictly monotonic in distance, near → 1.0, asymptote → 0.

Rationale for the jitter threshold: sub-pixel stability is invisible at
normal viewing distances; 1 px is the first visibly detectable discontinuity,
so the M1 pixel budget is set below it (0.5 px cross-path, 0 px stationary).

## Explicitly NOT covered by this bundle

- GPU/render-path jitter and depth (only analytic CPU depth here);
- real ephemerides / time scales (M7), planet LOD (M2+), Gaia (M9);
- the M0 device-matrix blockers remain: CI execution, macOS/MoltenVK,
  AMD/Intel devices, Gaia ADQL experiment, Earth-region selection.

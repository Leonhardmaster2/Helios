# Helios TerrainLab

TerrainLab is a deterministic, headless inspection tool for the v0.0.1
cube-sphere terrain contract. It invokes the production `TerrainGenerator`
and residency state machine; it does not contain a second terrain
implementation, open a window, or require a GPU.

For one selected 33x33 terrain tile it writes:

- `height.ppm` — generated height, using the same scale as the parent view;
- `normal.ppm` — world-space XYZ normal encoded as RGB;
- `slope.ppm` — angle between the displaced-surface normal and radial up;
- `parent_height.ppm` — the field limited to the parent tile's octave band;
- `residual.ppm` — signed child-minus-parent detail (blue/negative,
  red/positive);
- `error.ppm` — absolute residual divided by the analytic geometric-error
  bound;
- `report.json` — generator provenance, scalar metrics, stable hashes, exact
  seam and parent-compatibility checks, output checksums, timings, and a
  frame-by-frame bounded residency trace.

Image rows are written north-up and enlarged with nearest-neighbour sampling
by `--scale` so every source terrain sample remains identifiable. Height and
parent-height share one observed colour range. Residual/error views use the
generator's analytic parent-relative error bound, so their colours are
comparable rather than independently normalized.

## Build and run

Add the directory from `examples/CMakeLists.txt`:

```cmake
add_subdirectory(terrain_lab)
```

Then build and run it headlessly:

```powershell
cmake --build build/windows-dev --config RelWithDebInfo --target helios_terrain_lab
build/windows-dev/examples/terrain_lab/RelWithDebInfo/helios_terrain_lab.exe `
  --face +Z --level 7 --x 63 --y 61 `
  --seed 0x48454c494f535631 --amplitude 9000 `
  --root-octaves 4 --octaves 14 `
  --output-dir evidence/terrain_lab
```

Run `helios_terrain_lab --help` for the complete option list. The generated
terrain values, PPM bytes, tile content hash, and view checksums are identical
for a fixed configuration. Wall-clock timings in the report are intentionally
identified as non-deterministic performance observations.

## What the validation proves

The report verifies that:

- regenerating the tile reproduces its FNV-1a content hash;
- every sampled child residual is inside the analytic error bound;
- normals are finite, unit length to the reported tolerance, and outward;
- selected same-face neighbours share bit-identical edge samples;
- the canonical +X/-u to +Z/+u cube-face seam is bit-identical;
- a child tile's even samples reproduce its parent-band height exactly; and
- a scripted residency sequence never exceeds resident-count, CPU-byte,
  upload-byte, request, or pending-work budgets while demonstrating request
  throttling, upload throttling, and priority eviction.

This is a content/topology/residency proof, not yet a terrain material,
erosion, atmosphere, streaming-I/O, or Vulkan rendering demonstration.

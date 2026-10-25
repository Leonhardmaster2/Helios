# Helios v0.0.1 — release notes

**Status: MVP technical preview.** The automated headless gates pass. The interactive planet
route still needs a recorded visual/driver acceptance run, and the checked-in evidence bundles
predate this commit. The measurable contract and release gates are in
[V0_0_1_MVP.md](V0_0_1_MVP.md).

> **Attribution.** All, if not most, of the code in this repository was written with Opus 5.5.

## Why this release matters

Helios is an open-source, real-time Solar-System visualization engine and reference explorer,
written in C++20 with Vulkan through a statically linked Diligent backend. v0.0.1 is the first
build where the separate pieces meet: a planetary renderer that holds its precision from
astronomical distance to eye height, a scientific black-hole laboratory with a real-time
counterpart checked against it, and a deep-sky set whose structure is computed rather than
painted.

It is a technical demonstration, not a finished Solar-System simulator. The limits are listed
below and in the per-lab READMEs.

## Highlights

### 1. One continuous descent, 10⁹ m to 2 m

`helios_planet_lod_lab` starts roughly a billion metres from an airless procedural world and
flies to a two-metre eye height without a loading screen. The route is logarithmic
(`1e9 m → 1e8 m → 2e7 m → 1000 km → 100 km → 10 km → 1 km → 100 m → 10 m → 2 m`).

- A cube-sphere quadtree selects 33×33 patches by screen-space error, with separate split/merge
  thresholds, hysteresis, a hard leaf budget, and a documented reason for every budget-limited
  decision.
- The CPU keeps frame-tagged binary64 world positions and uploads only camera-relative float32
  patch anchors; the camera can reparent between root and body-fixed frames without a visible
  jump.
- Reversed-Z with an infinite far plane and a queried 32-bit float depth format.
- ImGui exposes the LOD policy, representation thresholds, wireframe, face/SSE diagnostics,
  frame-time percentiles, draw/triangle/leaf counts, and Vulkan/Diligent validation counts.
  Settings save and load through a schema-checked JSON file.
- The windowless `--capture` route renders scripted-route frames without a window or swap chain
  and exits non-zero if the validation layer reports an error.

### 2. Deterministic terrain and a measured precision boundary

`helios_terrain_lab` runs the production generator and residency state machine headlessly and
writes height, normal, slope, parent-height, residual, and error images plus a report. For a fixed
configuration it reproduces the tile content hash and every output checksum, verifies bit-identical
same-face and cube-seam edge samples, verifies that child even samples reproduce the parent band,
and checks that a scripted residency sequence never exceeds its resident-count, CPU-byte,
upload-byte, request, or pending-work budgets.

`helios_diligent_precision_lab` is the large-coordinate handoff proof. It builds binary64
fixtures from near-planet to one-light-year scales, subtracts the camera origin on the CPU,
uploads only bounded camera-relative float32 values, runs the view transform in one eight-thread
Diligent compute dispatch on Vulkan, and compares the readback with a binary64 reference. Its JSON
report also computes the deliberately wrong `float(world) − float(camera)` baseline to show why
the order of operations is not optional.

### 3. A real-time black hole validated against the CPU reference

Two executables share one model:

| Target | What it is |
|---|---|
| `helios_black_hole_lab` | Headless CPU reference: adaptive binary64 null geodesics, image and JSON report |
| `helios_black_hole_realtime` | Diligent/Vulkan renderer: per-pixel float32 geodesics, HDR glare, tone map |

Every camera sample is traced backward through the exterior Schwarzschild metric. The shadow,
the lensed far side of the disk, the secondary image under the hole, the photon ring, and the
warped star field are consequences of the integrated geodesics, not screen-space effects. The
disk uses gas on prograde circular geodesics with the Page–Thorne zero-torque flux for `a* = 0`,
electron-scattering limb darkening, and an exact frequency ratio built from the photon's conserved
energy and axial angular momentum, so Doppler beaming and gravitational redshift are applied per
emitting element. Colour and absolute luminance come from one Planck × CIE 1931 table shared by
the CPU and GPU paths.

`--validate` renders a geometry record on the GPU, traces the same pixel-centre rays with the
binary64 CPU reference, and compares class, asymptotic sky direction, and disk-crossing position.
On the reference machine (RTX 5080, 320×180, default camera): **57,584 pixels compared, 0 class
mismatches**, escape-direction error median **2.0e-6 rad** (maximum 6.5e-5), disk-radius error
median **7.4e-6 M** (maximum 3.9e-4 M). At 4K the real-time lab reaches 4.4 ms (225 fps) on
`ultra` and 3.0 ms (336 fps) on `balanced` with adaptive supersampling that concentrates extra
samples on the photon-ring band and high-contrast disk pixels.

The physics tier is explicitly Schwarzschild (`a* = 0`), stationary, isolated, uncharged vacuum,
with an optional idealized thin test disk. It is not Kerr, not GRMHD, and not an EHT simulation.

### 4. The Deep-Sky Lab: seven objects, none painted

`helios_deep_sky_lab` puts seven objects in one window, each with named variants after real
classes of object and a global Performance/Balanced/Ultra quality preset.

| Object | What is computed | What emerges |
|---|---|---|
| Emission nebula | GPU gas dynamics with photo-ionisation and a stretched grid | filaments, a glowing cavity, pillars and bright rims |
| Supernova remnant | GPU gas dynamics with a pulsar-wind source | blast wave, Rayleigh–Taylor fingers, a wind bubble |
| Disk galaxy | Particle-mesh N-body, up to 2.7 million star and gas particles | spiral arms, a bar, dust lanes, star-forming knots |
| Neutron star | Exact Schwarzschild light paths around a magnetised rotator | lensed surface, polar caps, beamed outflow, striped wind, accretion |
| Pulsar | The same model in its rotation-powered states | the lighthouse: beams bright only while they point at the camera |
| Black hole | The black-hole lab's renderer and disk model | shadow, photon ring, lensed and beamed disk |
| Planet | Cube-sphere terrain, scattering atmosphere, clouds, rings, moons | a planetary system with terrain statistics, ring gaps, and moon orbits |

The gas solver is a finite-volume Godunov scheme (MUSCL reconstruction with a van Leer limiter,
HLLC with an HLL fallback, two-stage Runge–Kutta) on a grid that is finest at the centre and
stretches towards the edges. `--validate-gas` checks it against Sod's shock tube (density mean
absolute error 0.0043, velocity 0.0079, pressure 0.0031 at 128³) and the Sedov–Taylor blast
(shock radius 1.6 % from the similarity solution; mass conserved to 3e-8, energy to 4e-7).

The galaxy is a particle-mesh N-body code with cloud-in-cell deposition, FFT gravity under
isolated boundary conditions, leapfrog stepping, star formation at the Kennicutt–Schmidt rate, and
supernova feedback. Star colours come from a Kroupa mass function summed over the population's
age; every star particle also carries individual stars with their planets and moons, and the
camera can fly from the whole disk down to a single world. `helios_galaxy_probe` runs the same
simulation without a GPU and reports the rotation curve, mode amplitudes, disk thickness, and
star-formation rate.

The planet is the one prescribed object: a height field with the statistics of real terrain, a
single-scattering atmosphere with a Hillaire-style multiple-scattering table, volumetric clouds
with two-stream reflectance and billow erosion, a ring slab shaded by single scattering with
Lindblad gaps at the moons' resonances, and moons on Kepler orbits. The same cube-sphere quadtree
as the planet lab draws it.

### 5. A Sun and the Solar System

`helios_solar_system` is the first Helios scene with a **Sun entity**. The Sun and all eight
planets are drawn by the planet lab's cube-sphere patch renderer: the Sun is a `PlanetBody` with
`star = true`, an emissive photosphere with Eddington limb darkening and drifting granulation in
`shaders/planet_patch.slang`, and each planet is the same body type with terrain or a gas-giant
band deck. Nothing is a billboard, a lens flare, or a skybox.

- Positions come from J2000 mean orbital elements (NASA NSSDCA fact sheets) and a two-body Kepler
  solver; the J2000 rows were checked against JPL Horizons DE441 to 2–20 arcseconds for the inner
  planets.
- Every body owns a quadtree selector and is rebased to its own centre before the float32 upload,
  so the camera can move from a whole-system map to a two-metre surface view without a scene load.
- The camera orbits the Sun or follows any planet; orbit lines and labels are drawn over the
  image. A windowless `--capture` route rasterises the same overlay into the PNG.
- Planet radii are exaggerated for display (500× by default) while the orbits stay true; a
  true-scale button sets both radii and the Sun back to 1×. The Sun has its own display scale so
  its disk does not swallow the inner planets.
- The 3-D scene renders at 4× for captures up to 1080p and 2× interactively, and an exact box resolve averages it
  down before the display chain; combined with a screen-space limb fade, the Sun's edge is
  anti-aliased at small sizes.
- Each body spins at its sidereal rotation period; the camera and sun direction are rotated into
  the body-fixed frame. Axial tilts are not applied yet.

The lab deliberately does not yet add Saturn's rings, moons, planetary atmospheres, perturbations
or an ephemeris-grade model; the requirements for those already exist in the planet/deep-sky code.

### 6. Diligent Engine, Vulkan only

The MVP applications use Diligent Engine's static Vulkan backend. Direct3D 11/12, OpenGL, Metal,
and WebGPU are disabled at configure time, and no Diligent runtime DLL is required. Slang shaders
are compiled offline to SPIR-V and embedded; reflection/ABI tests guard the shader/pipeline
contract. The pre-migration raw-Vulkan renderer is kept as an isolated parity target, and the two
Vulkan loaders are never linked into the same executable.

### 7. Evidence and a showcase site

The repository carries the M0 build/smoke evidence and the M1 precision bundle under `evidence/`.
`site/index.html` is a self-contained showcase with 4K captures of every deep-sky preset and a
short explanation of what each simulation computes.

## Performance snapshot

Windowless `--benchmark`, 3840×2160, RTX 5080, Balanced preset, simulations paused at their
default stop time:

| Object | Frame |
|---|---|
| Neutron star, isolated | 8.8 ms, 114 fps |
| Neutron star, millisecond pulsar | 5.4 ms, 185 fps |
| Neutron star, magnetar | 5.8 ms, 173 fps |
| Neutron star, accreting | 1.1 ms, 907 fps |
| Supernova remnant, Crab-like | 9.3 ms, 107 fps |
| Emission nebula | 10.2 ms, 98 fps |
| Galaxy, 2.16 million particles | 6.8 ms, 146 fps |
| Black hole, Balanced | 3.9 ms, 255 fps |
| Planet, Earth-like from orbit | 2.4 ms, 423 fps |
| Planet, Earth-like at eye level | 22.3 ms, 45 fps |
| Planet, ringed gas giant | 1.3 ms, 781 fps |

These are observations on one machine, not cross-vendor guarantees. The full table, including
Ultra/Performance presets, is in [examples/deep_sky_lab/README.md](../examples/deep_sky_lab/README.md).

## Build and run

```powershell
git submodule update --init --recursive
node scripts/fetch_deps.mjs --update-lock --write-cmake
cmake --preset windows-dev
cmake --build --preset windows-dev
ctest --preset windows-dev
```

Ordinary `ctest` is CPU/headless only. Physical-GPU checks are opt-in:

```powershell
cmake --preset windows-dev -DHELIOS_ENABLE_GPU_TESTS=ON
cmake --build --preset windows-dev
ctest --test-dir build/windows-dev -C RelWithDebInfo -L gpu --output-on-failure
```

Try the labs:

```powershell
build/windows-dev/examples/planet_lod_lab/RelWithDebInfo/helios_planet_lod_lab.exe
build/windows-dev/examples/solar_system_lab/RelWithDebInfo/helios_solar_system.exe
build/windows-dev/examples/solar_system_lab/RelWithDebInfo/helios_solar_system.exe `
  --capture solar_system.png --width 2560 --height 1440 --focus Earth --distance-au 0.0002
build/windows-dev/examples/deep_sky_lab/RelWithDebInfo/helios_deep_sky_lab.exe --object galaxy
build/windows-dev/examples/black_hole_lab/RelWithDebInfo/helios_black_hole_realtime.exe `
  --capture black_hole.png --width 1920 --height 1080 --preset ultra
```

## Limits and what is deliberately not claimed

- **The release gate is not closed.** The interactive planet route has not had a recorded
  visual/driver acceptance run, and the evidence bundles have not been regenerated at this commit.
- **The planet lab's body is airless procedural terrain**, not a measured DEM, erosion simulation,
  or streaming-I/O system. Skirts hide ordinary T-junctions; cross-face neighbour balancing is
  follow-up work. The deep-sky planet adds the atmosphere, clouds, rings, and moons.
- **The black hole is Schwarzschild only.** No Kerr spin, frame dragging, charge, GRMHD, radiative
  transfer, polarization, jets, or light-travel-time delay. The default 5200 K disk is a
  visualisation choice, not a measured temperature; turbulence is labelled presentation content.
- **The gas and galaxy models omit magnetic fields, self-gravity of the clouds, diffuse ionising
  radiation, and live dark matter.** Brightness ratios between emission processes are set by hand
  where no absolute calibration is carried through; the spatial patterns are computed.
- **The Solar-System lab is a visualization, not an ephemeris.** The planets use J2000 mean
  two-body elements (no perturbations, precession, Moon, or SPICE); planet radii are exaggerated
  for display; Saturn's rings, the moons, and the planets' atmospheres are not in this lab yet.
  The Sun is a limb-darkened, granulated emissive body, not a stellar-structure or MHD model.
- **The SDL-to-Diligent native-window bridge is Win32-only** for this release.
- **It is not a game.** There is no collision response, avatar, vehicle, fuel, orbital gameplay,
  vegetation, buildings, or water simulation.

## What comes next

1. Record the interactive descent acceptance run and regenerate the evidence bundles at the
   merge commit.
2. Cross-face neighbour stitching and asynchronous GPU terrain residency in the planet lab.
3. Saturn's rings, the Moon and the Galilean moons, and visible planetary atmospheres in the
   Solar-System lab.
4. A Linux/MoltenVK port that earns support through the same validation suite.
5. A named, provenance-backed Earth DEM region for the v1 hero body.
6. The remaining v1 milestones in [ROADMAP.md](../ROADMAP.md).

## Licensing

The intended community licence is AGPL-3.0-only, with a separately negotiated commercial licence
for Helios-owned code once the legal and operational pieces are ready. Third-party code, data,
catalogues, and assets keep their own licences; see [LICENSING.md](../LICENSING.md) and
[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md). The public legal rights-holder notice is still
a release blocker.

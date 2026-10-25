# Helios BlackHoleLab

Two executables render the same Schwarzschild (non-rotating) black hole with
the same physical model:

| Target | What it is | Display/GPU behaviour |
|---|---|---|
| `helios_black_hole_realtime` | Real-time renderer: float32 geodesics in one full-screen pass, HDR glare, tone map | Diligent/Vulkan. Interactive window by default; `--capture`, `--benchmark` and `--validate` are windowless |
| `helios_black_hole_lab` | Reference image generator: adaptive binary64 geodesics on CPU threads | Headless CPU, no GPU |

The CPU lab is the correctness anchor. The real-time lab is checked against
it per pixel (`--validate`), and both read the same black-body colour table,
so a real-time capture and a reference image of the same camera are directly
comparable.

## What is physically grounded

**Light paths.** Every camera sample is traced backward through the exterior
Schwarzschild metric from a static observer. The shadow, the lensed far side
of the disk, the secondary image under the hole, the photon ring and the
warped star field are consequences of the integrated null geodesics, not a
screen-space distortion.

- The CPU lab integrates `(r, dr/dλ, ψ)` with adaptive Dormand-Prince RK5(4)
  and classifies the first event: horizon, escape sphere, or thin-disk
  crossing.
- The real-time lab uses the orbital plane of each ray. With `u = M/r` and
  the orbital angle `φ` as the independent variable a null geodesic obeys the
  exact Binet equation `u'' = -u + 3u²`, integrated with fixed-step RK4. The
  disk plane meets the orbital plane along a line through the origin, so every
  disk crossing happens at `φ₀ + kπ`; steps land exactly on those angles, and
  the k-th crossing is the k-th order image. Rays end at `u ≤ 0` (infinity,
  which gives the asymptotic sky direction) or once they are ingoing inside
  the photon sphere.

**Disk radiometry** (`helios/blackhole/thin_disk.hpp`, `blackbody.hpp`).

- Gas on prograde circular geodesics, `Ω = M^(1/2) r^(-3/2)`, between the
  ISCO at `6M` and a chosen outer radius.
- Radial energy release from the Page-Thorne (1974) zero-torque solution for
  `a* = 0`. It vanishes at the ISCO and peaks near `9.55M`; the effective
  temperature is `T_peak (F/F_peak)^(1/4)`.
- The frequency ratio uses the photon's conserved energy and axial angular
  momentum, so it is exact for this emitter and a static camera:
  `g = sqrt(1 - 3M/r) / [sqrt(1 - 2M/r_cam) (1 - Ω L_z/E)]`.
  It contains the Doppler boost, gravitational redshift and time dilation.
- `I_ν/ν³` is invariant along a ray, so a black body at `T` is observed as a
  black body at `gT`. Colour and brightness both come from that one
  temperature through a table built by integrating Planck's law against the
  CIE 1931 colour-matching functions (absolute luminance in cd/m²).
- Electron-scattering limb darkening, `(1 + 2.06μ)`, flux-normalised, with
  `μ` measured in the rest frame of the gas.

## What is deliberately simplified or illustrative

- **`a* = 0` only.** No Kerr spin, frame dragging or ergosphere.
- **Razor-thin, optically thick disk.** No thickness, self-irradiation,
  returning radiation, colour-correction factor, GRMHD or radiative transfer.
- **Disk temperature.** Real disks peak in the ultraviolet or X-ray band and
  would look uniformly blue-white. The default peak of 5200 K is a deliberate
  choice that keeps the radial and Doppler temperature structure visible; it
  is a slider, not a measurement.
- **Turbulence (real-time lab).** A procedural emissivity field advected by
  Keplerian shear, which also breaks up the outer rim. It is presentation
  content, clearly labelled in the UI, and can be set to zero.
- **Sky.** Procedural stars and a procedural galactic band. Stars are
  unresolved point sources that keep their flux and are stretched by the
  lensing magnification; they are not a catalogue.
- **No light travel-time delay** between parts of the image, and a **static
  camera**: no orbital aberration.
- **Display.** Exposure, glare (a linear, threshold-free blur pyramid) and
  the ACES tone map are camera choices, not physics.

## Real-time lab

```powershell
cmake --build build/windows-dev --config RelWithDebInfo --target helios_black_hole_realtime
$exe = "build/windows-dev/examples/black_hole_lab/RelWithDebInfo/helios_black_hole_realtime.exe"

& $exe                                                   # interactive window
& $exe --capture out.png --width 1920 --height 1080 --preset ultra
& $exe --benchmark --width 2560 --height 1440 --report benchmark.json
& $exe --validate --width 320 --height 180 --report validation.json
```

Run `helios_black_hole_realtime --help` for every option (camera, disk,
individual relativistic terms, sky, exposure, quality).

**Interactive controls.** Left-drag orbits, right-drag looks around, the wheel
changes distance, Space pauses the disk, R resets the view, Escape exits. The
panel exposes the camera, the disk, each relativistic term, the sky, the
display transform, quality presets and live CPU/GPU timings, and can save and
load `helios_black_hole_settings.json`. No mouse grab, relative mouse mode,
global input hook or screen capture is used.

**Windowless routes.** `--capture`, `--benchmark` and `--validate` create a
Vulkan device without an SDL window or swap chain, so nothing appears on the
desktop. `--smoke-frames N` runs the interactive route in a window that is
never shown.

**Sampling.** The centre sample of each pixel is traced in uniform control
flow so its screen-space derivatives give filter footprints for the stars,
the galactic band and the disk structure. Extra samples are added everywhere
(`--spp`) and always in the thin band around the critical curve
(`--critical`), where the photon ring and shadow edge are sub-pixel.

**Validation.** `--validate` renders a geometry record instead of colour, reads
it back as float32, traces the same pixel-centre rays with the CPU reference,
and compares class (captured / escaped / first disk crossing), asymptotic sky
direction and disk crossing position. Pixels within `0.03M` of the critical
impact parameter are counted separately: there a ray orbits the photon sphere
and amplifies any perturbation exponentially. The route exits non-zero if the
published tolerances are exceeded.

Reference-machine result, 4 October 2026 (RTX 5080, uncommitted working tree,
320x180, default camera): 57,584 pixels compared, 0 class mismatches; escape
direction error median 2.0e-6 rad, maximum 6.5e-5 rad; disk radius error
median 7.4e-6 M, maximum 3.9e-4 M. For scale, one pixel at 4K with the
default field of view spans 3.1e-4 rad.

With turbulence, glare and the sky switched off, a real-time capture of the
CPU lab's default camera differs from the reference image by a median of one
8-bit level over the lit pixels; the remaining differences are the soft outer
rim and the dither.

### Adaptive supersampling and 4K performance

Supersampling every pixel is wasted on the sky and on the smooth face of the
disk. The trace pass therefore takes one sample per pixel and records what it
hit; a second pass (`shaders/black_hole_refine.slang`) re-traces only the
pixels that need it: the band around the critical impact parameter (the photon
ring and the shadow's edge), and disk pixels whose tone-mapped brightness
differs from a neighbour's by more than `--contrast` (default 0.05).
`--uniform-sampling` restores the old behaviour, and `--heatmap` shows where
the extra samples went. Against a 16-sample reference the adaptive image at
4 samples is as close as the uniform one (0.041 % against 0.035 % of pixels
off by more than 8 levels).

Measured windowless at 3840 x 2160 on an RTX 5080, default view
(`--benchmark --width 3840 --height 2160 --preset ...`):

| Preset | Steps / image orders / edge samples / critical samples | Frame |
|---|---|---|
| `ultra` | 48 / 4 / 8 / 12 | 4.4 ms, 225 fps |
| `balanced` | 32 / 3 / 4 / 8 | 3.0 ms, 336 fps |

With uniform supersampling `ultra` ran at 74-94 fps.

## CPU reference lab

```powershell
cmake --build build/windows-dev --config RelWithDebInfo --target helios_black_hole_lab
build/windows-dev/examples/black_hole_lab/RelWithDebInfo/helios_black_hole_lab.exe `
  --width 640 --height 360 --samples 2 `
  --output-dir evidence/black_hole_lab
```

It writes `black_hole.ppm`, `black_hole.rgba` (tightly packed RGBA8 for
regression tooling) and `report.json` with the model description, ray
classification, null-constraint residuals, performance and a pass/fail
validation status. The image bytes are deterministic for a fixed
configuration; wall-clock values in the report vary between runs. Run
`helios_black_hole_lab --help` for all controls.

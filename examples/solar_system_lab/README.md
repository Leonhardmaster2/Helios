# Helios Solar System lab

`helios_solar_system` is the first Helios scene with a **Sun entity**: the Sun
and the eight planets are all drawn by the planet LOD lab's cube-sphere patch
renderer, from the same `PlanetBody` and quadtree code that draws a planet
from orbit to eye level. The Sun is a body of the scene, not a light, a
background sprite, or a lens flare: it has a radius, a surface, a rotation and
a place in the quadtree, and it can be flown to and inspected like a planet.

```powershell
build/windows-dev/examples/solar_system_lab/RelWithDebInfo/helios_solar_system.exe
```

The default view is a tilted map of the inner system. Drag with the left mouse
button to orbit, use the wheel to change distance, and pick a focus body in
the panel. `WASD`/arrow keys move the camera, `Space` plays the clock, `[` and
`]` change focus, `R` resets the view and `Escape` exits. Orbit lines and
labels are drawn over the image; the Sun, each planet, and the orbit colour of
each planet are visible at a glance.

## What is computed

- **Positions.** Every planet is placed from its J2000 mean orbital elements
  (`solar_system_data.hpp`): semi-major axis, eccentricity, inclination,
  longitude of the ascending node, argument of periapsis, mean anomaly at
  J2000 and sidereal period. The solver is the two-body Kepler problem:
  `M = M0 + n t`, Newton for `E - e sin E = M`, then the true anomaly, radius
  and the three-element rotation into the ecliptic frame. The table is from
  the NASA NSSDCA fact sheets; the J2000 rows were checked against JPL
  Horizons DE441 to 2-20 arcseconds for the inner planets.
- **The Sun's surface.** A star body takes a separate branch in
  `shaders/planet_patch.slang`: Eddington limb darkening
  (`I(mu) = I(0)(1 - 0.6(1 - mu))`) and two octaves of drifting granulation,
  output as linear HDR radiance. It is tone-mapped and glared by the same
  `HdrPostChain` as the planets, so the Sun blooms without a special sprite.
- **Rotation.** Each body turns at its sidereal rotation period (retrograde periods are negative); the camera and sun direction are rotated into the body-fixed frame, so the surface moves under a fixed view. Axial tilts are not applied yet.
- **Each planet's surface.** Rocky worlds use the terrain height field and
  climate materials of `planet_surface.*`; the gas giants use the banded
  cloud-deck branch with their own band counts and colours. Earth keeps its
  ocean, ice and cloud-shadow layer.
- **Level of detail.** Each body owns a `QuadtreeLodSelector` sized for its
  display radius and the current camera. A capture settles the selection until
  a still camera produces no further splits or merges; the interactive route
  advances one selection per frame with hysteresis, as the planet lab does.
- **Camera-relative precision.** Positions are binary64 metres in the
  heliocentric frame, and each body's patch anchors are rebased to that body's
  centre before the float32 upload, so a camera at Neptune and a camera two
  metres above Mars use the same path.

## Scale

The real solar system is mostly empty: at true scale Earth is 1/12,000 of an
AU across, and a whole-system view shows a bright point and seven dust motes.
The lab therefore separates the scales:

| Control | Default | Meaning |
|---|---|---|
| Planet radius x | 500 | display exaggeration of every planet's radius |
| Sun radius x | 12 | display exaggeration of the Sun's radius |
| True scale | button | sets both to 1 |
| Camera distance | 2 AU | distance from the focused body |
| Orbit line fit | automatic | an orbit much larger than the view is not drawn |
| Supersample | 2 (4 available) | the 3-D scene is rendered at 2x or 4x and box-resolved to the output |

Orbits and positions are never compressed; only the radii are exaggerated.
The panel shows each body's true radius, period, rotation and distance from the
Sun.

## Anti-aliasing

The 3-D scene is rendered at twice the output resolution by default (the **Supersample** control: 1, 2 or 4; 4 is the smoothest and is available for captures) and an exact box resolve (`shaders/solar_resolve.slang` for 2x, `shaders/solar_resolve4.slang` for 4x) averages the samples into each output pixel before exposure, glare and tone mapping. The Sun's limb also gets a screen-space coverage fade in `shaders/planet_patch.slang`, so a small solar disk is not a staircase. The 2x scene also gives the quadtree twice the screen-space error budget, so the planets resolve more LOD detail.

## Routes

```powershell
# interactive window
helios_solar_system.exe

# a windowless capture
helios_solar_system.exe --capture solar.png --width 2560 --height 1440 `
  --days 0 --focus Earth --distance-au 0.0002 --planet-scale 1

# the Sun as the subject
helios_solar_system.exe --capture sun.png --focus Sun --distance-au 0.2 `
  --planet-scale 40

# a hidden-window smoke run of the interactive path
helios_solar_system.exe --smoke-frames 60
```

`--help` lists every option: `--days`, `--focus`, `--distance-au`,
`--elevation`, `--azimuth`, `--fov`, `--planet-scale`, `--sun-scale`,
`--true-scale`, `--exposure`, `--glare`, `--supersample`, `--width`, `--height`,
`--smoke-frames`, `--report PATH.json`, `--now`.

Captures are windowless (no SDL video subsystem, no swap chain). `--report PATH.json` writes the date, camera, display scales, body positions and radii, metrics, adapter and validation count for a reproducible record. The orbit and
label overlay is rasterised into the PNG with the PNG writer's 4x6 font, so a
headless image carries the same context as the interactive view. The process
exits non-zero if the Vulkan validation layer reports an error.

## What is not modelled

- **Two-body orbits only.** No perturbations, resonances, precession, the
  Moon, or SPICE kernels. The mean elements are a visualization aid, not an
  ephemeris.
- **Planet surfaces are illustrative.** Earth is procedural (not a DEM);
  Venus has no sulphuric-acid cloud deck; Mars has no polar caps beyond the
  snow material; the gas giants have banded cloud decks, not measured winds.
- **No rings or moons yet.** Saturn's rings, Jupiter's Galilean moons and
  Earth's Moon are the next additions; the planet system and ring code they
  need already exists in `planet_system.*` and `planet_sky.slang`.
- **No atmospheres on the planets in this lab.** Earth's cloud layer is used
  for its shadow on the ground, not as a visible cloud deck; the atmosphere
  pass of the deep-sky planet is a separate renderer.
- **The Sun is not a stellar model.** It is a limb-darkened, granulated
  blackbody-coloured surface with a display temperature, not a
  radiative-transfer or MHD simulation.
- **No collision, landing, or surface physics.** This is a camera and a
  renderer.

## Where the code is

| File | Contents |
|---|---|
| `solar_system_data.hpp` | J2000 elements, physical data, Kepler solver |
| `solar_system.hpp/.cpp` | Planet/Sun `PlanetBody` presets, names, positions, display radii |
| `solar_system_stage.hpp/.cpp` | multi-body LOD, camera, scene targets, HDR post chain |
| `main.cpp` | window, capture, orbit/label overlay, UI |
| `shaders/planet_patch.slang` | the star branch and the existing terrain/gas-giant shading |

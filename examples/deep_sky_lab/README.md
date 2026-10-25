# Deep-sky lab

Seven objects in one window, none of them painted. The simulated ones show
the current state of a run that starts from something featureless; the
structure you see is what the physics made of it. The others are exact
light paths through models whose parameters are physical.

| Object | What is computed | Starts as | What emerges |
| --- | --- | --- | --- |
| Emission nebula | gas dynamics on the GPU with photo-ionisation | a round, uniform, turbulent cloud | filaments, then a glowing cavity with pillars and bright rims |
| Neutron star | exact light paths around a magnetised, rotating star | – | lensed surface, polar caps, beamed outflow, striped wind, accretion streams and disk |
| Disk galaxy | N-body: up to 2.7 million star and gas particles, gravity on a mesh, a central black hole; every particle the stars it stands for, each with its planets and moons | a smooth, axisymmetric disk | spiral arms, a bar, dust lanes, star-forming knots; from inside, the band of the galaxy and its single stars |
| Supernova remnant | gas dynamics on the GPU | smoothly expanding ejecta | a blast wave, Rayleigh-Taylor fingers, a pulsar wind bubble |
| Black hole | exact Schwarzschild geodesics, Page-Thorne disk | – | shadow, photon ring, lensed and beamed disk |
| Planet | a planetary system on the planet lab's cube sphere: height-field terrain, single-scattering atmosphere, volumetric clouds, rings, moons on Kepler orbits | – | the one prescribed object: terrain with the statistics of real relief; ring gaps at the moons' resonances; the bodies' shadows on each other |
| Pulsar | the neutron star in a rotation-powered state | – | the lighthouse: beams that are bright only while they point at you |

```bash
helios_deep_sky_lab --object galaxy
```

Routes: an interactive window (default), `--capture PATH.png` and `--benchmark`
(both windowless: no window, no swap chain, nothing on the desktop), and
`--validate-gas`. `--help` lists every option; `--background` runs at low
process priority and `--threads N` limits the CPU threads of the galaxy.

## Variants and presets

Every object has named variants: sets of initial conditions (or, for the
black hole, disk parameters) after a class of real objects, each with a view
that shows it. They are a starting point; every parameter stays editable.
The panel of each object lists them, `--variant NAME` picks one on the
command line, and `object_variants.cpp` / `neutron_star_model.cpp` say what
each assumes and cites what it is modelled on.

| Object | Variants |
| --- | --- |
| Nebula | H II region (Orion-like); pillars (Eagle-like); blister and champagne flow; reflection nebula (B star) |
| Remnant | core-collapse with pulsar; Type Ia in thin gas (Tycho-like); pulsar-wind-nebula dominated (Crab-like); old remnant with a radiative shell (Cygnus Loop-like) |
| Galaxy | Milky-Way-like (Sb, with Sagittarius A*); barred, disk-dominated (SBb); gas-rich flocculent (Sc); early-type, bulge-dominated (Sa); the other three with central black holes from the black hole-bulge mass relation (Kormendy & Ho 2013) |
| Black hole | cool disk (visualisation); stellar-mass X-ray binary; supermassive quasar disk; bare hole |
| Planet | Earth-like world with the Moon; desert world (Mars-like, two small moons); ice world with an icy moon; ringed gas giant (Saturn-like, four moons) |
| Neutron star | isolated cooling star; young pulsar; millisecond pulsar; magnetar; accreting millisecond pulsar; orthogonal rotator; nearly aligned rotator |
| Pulsar | the rotation-powered states of those |

A global **quality preset** (Performance, Balanced, Ultra; `--preset`) sets
the gas grid (192, 256 or 320 cells per edge), the ray-march samples, the
galaxy's particle counts (1.1, 2.2 or 2.7 million; applied at its next
restart), the black hole's integration, the neutron star's step size, the
planet's level of detail and its sky and cloud samples, and how many still
frames are averaged.

## Galaxy

`galaxy_simulation.*` is a particle-mesh N-body code: cloud-in-cell mass
deposit, FFT convolution with the softened Newtonian kernel under isolated
boundary conditions, leapfrog time stepping. It runs on its own thread; the
renderer draws whatever state is newest.

- **Stars** are collisionless. The disk starts in equilibrium (Toomre Q = 1.3)
  with no arms; swing-amplified density waves grow from the particle noise.
- **Gas** is an isothermal fluid of particles: the same gravity, the pressure
  of a turbulent interstellar medium, and inelastic relaxation towards the
  local mean flow. It piles up in the arms and turns into stars at the
  Kennicutt-Schmidt rate; the momentum of supernovae (3000 km/s per unit mass
  of stars formed) pushes it apart again and leaves holes.
- **Colour** is the age of the stars. `stellar_population.*` sums black bodies
  over a Kroupa mass function, with evolved stars from the fuel-consumption
  theorem: a population a few Myr old is blue and 200 times brighter per unit
  mass than a 10 Gyr old one, which is yellow-orange.
- **Dust** is the simulated gas with the Milky Way's dust-to-gas ratio; each
  particle is dimmed and reddened by the gas between it and the camera.
- **The disk's gravity in three dimensions.** The mass is deposited on the
  mesh in the plane; a star feels the potential of that surface density seen
  from an effective height a = eps + h ln cosh(z / h) (the softening length
  in the plane, growing like |z| above a layer of thickness h). Both the
  in-plane and the vertical force are the gradient of that one potential,
  tabulated at six heights from 0.2 to 6.4 kpc (each one inverse FFT, which
  yields the potential and the vertical pull together) and interpolated in
  ln a. A force that is the gradient of a potential conserves energy. The
  earlier model took the in-plane pull at z = 0 for every star, and the stars
  the bar funnelled through the centre were pumped kiloparsecs above it: the
  disk inside 0.5 kpc thickened from 0.27 to 1.1 kpc (rms) in 2 Gyr. Now it
  thickens slowly, to 0.43 kpc after 13 Gyr (`helios_galaxy_probe`, 1.1
  million particles).
- **Central black hole**: a point mass in the potential, 4.3 million solar
  masses for the Milky Way (GRAVITY Collaboration 2022). It is softened over
  the same 50 pc as the bulge's core, so its sphere of influence (a few
  parsecs) is not resolved and it barely changes the orbits; the mesh cannot
  follow anything smaller. Orbits through the stiff centre are integrated in
  sub-steps.
- **Bulge**: isotropic velocities from the Jeans equation of the truncated
  Hernquist profile, with no star faster than it takes to climb to the
  truncation radius (Gaussian tails used to send a few per cent of the bulge
  out to tens of kpc).
- **Fixed, not simulated**: the dark halo, the bulge's gravity and the black
  hole are rigid potentials (the bulge's stars orbit in them).
- **Runs to 16 Gyr.** The birth times of the stars are encoded up to that
  age. With no gas falling in from outside, the disk uses up most of its gas
  in the first few Gyr and its star formation then fades, so blue arms are a
  feature of the first billions of years.

`helios_galaxy_probe` runs the simulation without a GPU and prints the
quantities an image cannot show: the rotation curve, the strength of the
m = 1-4 modes, the disk's thickness by population and radius, the star
formation rate, and the step time split into deposit, field and kick.
`--systems` prints checks of the stars and planets below.

### Every star, with its planets

A star particle stands for tens of thousands of stars born together
(37 500 solar masses at the Balanced preset: 64 714 stars). `stellar_systems.*`
names each of them, and `shaders/lib/stellar.slang` computes the same on the
GPU:

- **Mass**: star r of a particle is its r-th most massive; its initial mass
  is where the upper tail of the Kroupa (2001) mass function holds r + u of
  the particle's stars (u from a hash). The particle's stars are the mass
  function itself, without sampling noise.
- **State** from the initial mass and the particle's age, with the
  relations of the population light (so a particle's light is the sum of its
  stars, to a few per cent): the main sequence brightening by half; the giant
  branch radiating the energy of the fuel-consumption theorem over the
  luminosity function of real red giants; white dwarfs (initial-final mass
  relation of Kalirai et al. 2008, Mestel cooling), neutron stars and black
  holes.
- **Place**: a Gaussian offset from the particle's centre over the distance
  its stars are spread (the width of its image), which moves with it.
- **Planets**: three or four, from a disk of solids following the
  minimum-mass extrasolar nebula of Chiang & Laughlin (2013), scaled with the
  star's mass and metallicity (the disk's age-metallicity relation, or the
  bulge's), twice as dense beyond the snow line (Ida & Lin 2004). Orbits at
  the period ratios of the Kepler multi-planet systems, at least ten mutual
  Hill radii apart; masses from the solids of each planet's annulus; cores of
  ten Earth masses that grow before the gas is gone become gas giants
  (Pollack et al. 1996); radii from Chen & Kipping (2017); equilibrium
  temperatures from the star's present luminosity; kinds and albedos by
  composition and temperature, the giants by the cloud classes of Sudarsky,
  Burrows & Hubeny (2000). Giant stars swallow the planets inside twice their
  largest radius; mass loss widens the orbits of the rest; a supernova
  unbinds them.
- **Moons** wherever 0.4 Hill radii (Domingos et al. 2006) leave room beyond
  the Roche limit: two to four regular satellites of the giants, about 10^-4 of
  their mass (Canup & Ward 2006); one or two impact-made moons of the rocky
  planets. Icy rings around cold gas giants.

Near the camera the brightest stars of each particle are drawn one by one
(`shaders/galaxy_resolved_stars.slang`): every star whose image would reach a
hundredth of display white, nearest particles first, up to a budget (1.5
million by default), and each particle's image keeps only the light of the
stars not drawn. From inside the disk the galaxy becomes a band of light with
single stars over it.

A visited system is traced as spheres (`shaders/star_system.slang`): the star
as a limb-darkened black body, planets and moons lit by it with eclipses
(umbra and penumbra from the overlap of the disks), molten day sides glowing
as black bodies, icy rings as a single-scattering slab, and bodies smaller
than a pixel as the points of light they reflect.

### Exploring

Click a star particle to fly to it; closer in, click a single star, then a
planet or a moon (or use the lists in the panel). Backspace backs out one
level; the panel also flies to the whole galaxy, the central black hole, and
the first Sun-like star, ringed giant, lava world, red giant or white dwarf
near 8 kpc. The camera follows its focus as the galaxy turns. Time runs at the
speed set for the galaxy view (Myr per second) scaled by how long what is in
view takes to change: megayears per second for the whole galaxy, kiloyears
among the stars, a year per second in a planetary system, hours per second
beside a planet. Exposure adapts like an eye: to the galaxy, to a star's
surface, or to what a star lights. Orbits and names are drawn over the image
(they are labels, not light).

```bash
helios_deep_sky_lab --object galaxy --galaxy-focus sunlike
helios_deep_sky_lab --object galaxy --galaxy-focus ringed --sim-time 3000 --capture ringed.png
```

## Emission nebula and supernova remnant

`hydro_simulation.*` drives the compute passes in `shaders/hydro_*.slang`: a
finite-volume Godunov scheme for the Euler equations (piecewise-linear
reconstruction with the van Leer limiter, HLLC Riemann solver falling back to
HLL in cells flagged as strong shocks, two-stage Runge-Kutta) with time steps
from the Courant condition.

**The grid is stretched.** Along each axis the cell index maps to position
as `x = L sinh(a i) / sinh(a)`: cells are finest at the centre, where the
object is, and grow smoothly towards the edges (by `cosh(a)`, 3.8 times for
the nebula's a = 2). The same 256 cells then cover a box several times wider
than the fine region, so what flows out of the fine region is still followed,
at lower resolution, instead of leaving the simulation; the ionised gas of
the nebula no longer shows the box. The display takes steps that follow the
local cell size. The reconstruction is done in index space, which costs
accuracy where the stretching is strong; the validation runs use a = 0.

`--validate-gas` checks the solver against two exact solutions:

| Problem | Result at 128^3 |
| --- | --- |
| Sod's shock tube, mean absolute error | density 0.0043, velocity 0.0079, pressure 0.0031 |
| Sedov-Taylor blast, shock radius | 1.6 % from the similarity solution |
| Sedov-Taylor blast, conservation | mass to 3e-8, energy to 4e-7 |

**Nebula.** A molecular cloud with a random velocity field and no star.
Until `ignition` only the turbulence acts. Then one massive star switches
on: `hydro_rays.slang` walks outwards from it along every direction and
records where recombinations have used up its ionising photons (the
Stromgren condition). Gas inside that surface is held at 10^4 K, everything
else at 30 K, and the pressure difference does the rest. The colours follow
from the ionisation parameter: [O III] near the star, [N II] and [S II] at
the dense edges, starlight scattered by dust around the star (scaled by the
star's own visible light, so a B star makes a reflection nebula). The star
can sit off-centre (a blister that bursts out of its cloud).

**Remnant.** Ejecta of a few solar masses and up to 10^51 erg (Chevalier's
flat core and power-law envelope, with a chosen envelope index) in gas of a
chosen density, with density fluctuations laid out at a scale the grid
resolves. A pulsar, if there is one, adds energy at the centre at its
spin-down rate, and the energy it has put out before the run's start sits in
a bubble from the beginning. Light comes from four sources, all functions of
the simulated state: bremsstrahlung of the shocked gas, lines of gas that has
cooled, the Balmer lines of the blast wave, synchrotron light of the wind
bubble. In an old remnant the swept-up gas cools too, once its shock has
slowed below about 230 km/s, and collapses into the radiating shell of the
radiative phase.

## Neutron star and pulsar

`shaders/neutron_star.slang`, parameters in `neutron_star_model.*`. Light
paths are exact Schwarzschild geodesics. Everything drawn follows from the
dipole field, the spin and a few physical parameters:

- **Surface**: a black body whose temperature pattern follows from the
  crust's anisotropic conduction (hottest at the magnetic poles), with the
  gravitational redshift and the Doppler shift of the rotation; polar caps
  heated by the return current; hot spots where accreted matter lands.
- **Magnetosphere**: plasma on the open field lines radiates along its
  motion, so a beam is bright only while it points at you (the pulse);
  beyond the light cylinder the field is wound into the striped wind, whose
  current sheet (the split-monopole surface) is drawn as a thin emitting
  sheet, integrated analytically across each step; a magnetar's twisted
  bundle glows.
- **Accretion**: a disk truncated at the magnetospheric radius, streams along
  the loaded field lines to the poles, the disk heated by viscosity and by
  the X-rays of its inner parts out to its tidal edge (Kepler's law, Eggleton
  1983, Whitehurst 1988; irradiation after de Jong et al. 1996).
- **Bands**: visible light, or the 0.2-10 keV band shown as colour, where the
  temperature pattern of the surface appears.

The pulsar entry is the same model restricted to the rotation-powered
states, with its own settings and clock.

## Black hole and planet

The black hole is the black-hole lab's renderer (`examples/black_hole_lab`)
with its disk set by the variant: a cool disk that shows its colour
structure, or the Page-Thorne peak temperature a 10 or 1e8 solar-mass hole
reaches at a tenth of the Eddington rate (`object_variants.cpp`).

The planet is a planetary system drawn with the planet LOD lab's cube-sphere
quadtree (`planet_stage.*`; `examples/planet_lod_lab/planet_system.*` and
`planet_surface.*`): one body with its rings, its cloud layer and up to four
moons, each moon a body of its own, tidally locked, on a circular Kepler
orbit set by the planet's mass. It is the one object whose structure is
prescribed rather than grown. The relief is a height field with the
statistics of real terrain (the bimodal hypsometry of continents and ocean
floors, ridged multifractal mountain belts after Musgrave, craters with the
depth and rim ratios of Pike 1977 and Melosh 1989), evaluated identically on
the CPU in double precision and on the GPU in float with patch-local lattice
anchors down to octave 22, six metres on an Earth-sized body. Computed on
top of it:

- **Atmosphere**: scattering of sunlight by molecules (Rayleigh,
  coefficients of Bucholtz 1995) and aerosols (Mie, Cornette-Shanks) along
  the view ray, with the optical depth to the sun from a precomputed table
  over height and sun elevation, and the multiply scattered light from a
  second table built the way Hillaire (2020) does: the light scattered once,
  gathered over all directions with an isotropic phase together with what a
  ground of albedo 0.15 returns, divided by one minus the fraction that
  scatters again. Aerial perspective on the ground, the sun's disk and the
  lab's star field behind. Not modelled: ozone (so the zenith is a little
  less deep a blue than Earth's), polarisation.
- **Clouds**: a layer between a base and a top height whose cover is a
  domain-warped noise field on the sphere (thresholded at the normal
  quantile of the cover fraction), carried by the zonal wind and eroded by
  billows a few kilometres across (Schneider & Vos 2015). Where the billows
  fall below the pixel footprint they are not averaged into a haze: a far
  pixel sees the fraction of opaque billows its cover implies, the mean of
  the transmittance rather than the transmittance of the mean, so cloud
  fields thin out at their edges as real ones do. Ray-marched with
  Beer-Lambert extinction: the direct sun scattered once through a two-lobe
  Henyey-Greenstein phase, plus the multiply scattered light of the layer
  as the reflectance of a conservative slab in the two-stream approximation
  (Meador & Weaver 1980), falling linearly from the lit face to the diffuse
  transmission of the far face. Thick stratocumulus so reflects 60 to 75 %
  of sunlight, the observed albedo; a single-scattering march alone would
  leave the clouds grey. The billows shadow the ground.
- **Rings**: a slab in the equatorial plane with a normal optical depth over
  radius, shaded by single scattering (Chandrasekhar's slab solution for the
  lit and the unlit side) with a backscattering phase. Gaps open at the
  inner Lindblad resonances of the moons (Goldreich & Tremaine 1978, 1980;
  widths scaled as (m/M)^(2/3) a and calibrated to Mimas and the Cassini
  division) and along the orbit of any moon inside the rings; ringlets stand
  in for the viscous overstability of dense rings (Schmit & Tscharnuter
  1995). The rings shadow the planet, the planet shadows the rings and its
  moons. The Roche limit for ice is shown next to the ring controls.
- **Moons** have their own terrain presets (a cratered rocky moon, a bright
  icy one) and orbit with the mean motion of the planet's mass; their periods
  and Hill radii are shown.
- **Time** moves the sun with the planet's rotation, the clouds and the
  moons; the lab runs it at 600x when animating and the camera stays
  fixed to the ground.
- **Controls**: the planet panel has the preset combo, the surface (land
  bias, mountains, sea level, craters, temperature, seed), the sun and the
  day length, the clouds (cover, base, top, extinction, wind), the rings
  (edges in radii, optical depth, albedo, ringlets, with the Roche limit for
  ice shown) and the moons (count, orbit, radius, with periods and Hill
  radii). On the command line `--variant` picks a preset and `--rings
  on|off`, `--clouds on|off` and `--moons N` override it; `--report` writes
  the ground elevation under the camera with the rest.
- **Known limits**: the terrain's shading follows the level of detail (the
  normal is the height gradient at the tile's vertices), so from a hundred
  kilometres up the relief reads as colour more than as shade until the
  tiles split; Ultra sharpens it. The cloud base is a height above the sea
  level, so a 3 km plateau sits in the cloud deck. The moons cast no shadow
  on the planet (eclipses), and the rings' shadowed side is black (no
  planetshine).

## What is assumed rather than computed

- **Initial conditions**: the galaxy's mass model, the cloud's size and
  turbulence, the explosion's energy. Nothing that follows is prescribed.
- **Colour is what the eye would see** (CIE 1931 observer). Photographs of
  nebulae are redder because cameras respond to H-alpha far better than eyes
  do; to the eye [O III] makes ionised gas teal. Faint objects (a reflection
  nebula, a Type Ia remnant) raise the exposure, as a long photographic
  exposure would.
- **Brightness scales** of each emission process relative to the others are
  set by hand where no absolute calibration is carried through (scattered
  starlight in the nebula; bremsstrahlung, rim and synchrotron in the remnant;
  the neutron star's non-thermal components). The spatial pattern of each is
  computed.
- **Resolution**: a newborn cluster's birth cloud (Charlot & Fall's optical
  depth), the thickness of an ionisation front, the knots that cooled ejecta
  collapse into, and the radiative shell of an old remnant are all smaller
  than a cell and are represented by a floor or a closure, named in the
  source where it is applied. The ejecta of a slow, Crab-like explosion span
  only a few central cells, which shows in the shape of its bubble.
- **Missing physics**: magnetic fields (no pulsar jet or torus), self-gravity
  of the gas clouds, diffuse ionising radiation, live dark matter; for the
  neutron star frame dragging, oblateness and light travel time.

## Display

The simulations and the picture are kept apart so that neither makes the
other stutter.

- **Gas volumes.** The simulation gives every cell one value, and the
  brightest structures are a cell or two thick; drawn raw they show the mesh
  as blocks and combs. `hydro_smooth.slang` therefore applies a (1 2 1)
  binomial filter along each axis before display, which removes cell-to-cell
  alternation and keeps the total light; the display pass then interpolates a
  band-limited field with steps that follow the stretched grid's cell size.
- **Gas time steps** are queued for the GPU and never waited for: the Courant
  time step uses the newest signal-speed measurement that has come back, with
  a margin. As many steps are taken per frame as fit the "simulation GPU
  budget" (5 ms by default).
- **Galaxy.** The run publishes a state every few hundredths of a second. It
  is uploaded a few megabytes per frame into a second buffer and swapped in
  when complete; every particle carries its velocity and is drawn where it is
  at the display time, so the motion is smooth at any frame rate.
- **Stars** over a gas volume are images of a point through a telescope
  (core and diffraction spikes) that fade to zero at their edge; the dust in
  front of each is integrated once per star and frame.
- **Sky.** Background stars are point sources whose images are mapped
  through the lensing's Jacobian, so a lensed star stays a dot and gains the
  magnification instead of being smeared.

The window runs without the Vulkan validation layers (`--validation on` to
enable them); the windowless routes keep them on.

## Performance

Windowless `--benchmark` at 3840 x 2160 on an RTX 5080, Balanced preset,
with each simulation at its default stop time (simulations paused):

| Object | Frame |
| --- | --- |
| Neutron star, isolated | 8.8 ms, 114 fps |
| Neutron star, young pulsar / pulsar entry | 8.8 ms, 114 fps |
| Neutron star, millisecond pulsar | 5.4 ms, 185 fps |
| Neutron star, magnetar | 5.8 ms, 173 fps |
| Neutron star, accreting | 1.1 ms, 907 fps |
| Supernova remnant (256^3 stretched, 160 samples), Crab-like | 9.3 ms, 107 fps |
| Emission nebula (256^3 stretched, 160 samples) | 10.2 ms, 98 fps |
| Galaxy (2.16 million particles) | 6.8 ms, 146 fps |
| Black hole, Balanced | 3.9 ms, 255 fps |
| Planet, Earth-like from orbit | 2.4 ms, 423 fps (Ultra 3.9 ms) |
| Planet, Earth-like 400 km up | 7.5 ms, 134 fps |
| Planet, Earth-like at eye level under cloud | 22.3 ms, 45 fps |
| Planet, ringed gas giant | 1.3 ms, 781 fps |
| Planet, desert world | 2.9 ms, 346 fps |

Ultra (320^3 gas, 208 samples) runs the nebula at 9.7 ms (103 fps) at 4K;
Performance (192^3, 112 samples) at 4.5 ms (221 fps).

In the window, while a simulation is still running (`--smoke-frames N --vsync
off` prints these): a gas time step at 256^3 costs 3 to 5 ms on the GPU and
the frame takes what the simulation budget allows plus the drawing; a galaxy
time step (0.5 Myr) about 30 ms on the CPU, off the thread that draws.

Other programs that draw on the same GPU (a game, an animated wallpaper)
take their share in slices of several milliseconds, which shows up here as
single slow frames.

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// N-body simulation of a disk galaxy.
//
// Nothing here describes what a galaxy looks like.  The run starts from a
// smooth, axisymmetric, rotating disk of stars and gas in equilibrium, with
// no arms, no bar and no dust lanes; whatever structure appears afterwards is
// made by gravity acting on the particles:
//
//   stars   Collisionless particles.  Their mutual gravity is solved on a
//           mesh (particle-mesh: cloud-in-cell deposit, FFT convolution with
//           the softened Newtonian kernel under isolated boundaries,
//           leapfrog).  Swing-amplified density waves grow from the particle
//           noise into spiral arms.
//   gas     Particles of an isothermal fluid: the same gravity, plus the
//           pressure of a turbulent interstellar medium and inelastic
//           relaxation towards the local mean flow (shocks).  Gas therefore
//           piles up in the arms, where it is dense enough to turn into stars
//           at the observed Kennicutt-Schmidt rate.  The young stars push
//           back: the momentum of their supernovae and winds acts as a
//           pressure that disperses the clouds they formed in.
//   bulge   Old stars orbiting in three dimensions in the fixed potential of
//           the bulge and halo.
//   nucleus A central black hole, a point mass in the same fixed potential.
//   halo    Dark matter: a fixed logarithmic potential, and invisible.
//
// A star particle carries its birth time; its colour and brightness follow
// from its age alone (stellar_population.hpp).  Blue arms, pink H II regions
// and dust lanes are consequences, not inputs.
//
// Gravity of the disk.  The mass is deposited on a mesh in the plane, and the
// field is evaluated at the star's height through one potential,
//
//   Phi(x, y, z) = Sigma * K(a),  K(a) = -G / sqrt(R^2 + a^2),
//   a(z) = eps + h ln cosh(z / h),
//
// the potential of the surface density at an effective height a: the
// softening length in the plane, growing like |z| above a layer of thickness
// h.  Both the in-plane and the vertical force are its gradient (the vertical
// one is 2 pi G times the surface density seen through the Poisson kernel at
// height a, times tanh(z / h)), tabulated at heights eps * 2^k and
// interpolated in ln a.  A force that is the gradient of one potential
// conserves energy; an earlier version took the in-plane pull at z = 0 for
// every star, and stars that the bar funnelled through the centre were
// pumped to kiloparsecs above it within two billion years.
//
// Approximations: the disk's own gravity sees the disk as a thin sheet;
// bulge, halo and black hole do not respond to the disk; the black hole is
// softened over the same 50 pc as the bulge, so its sphere of influence (a few
// parsecs) is not resolved; supernova feedback is a pressure, not resolved
// explosions; no cooling, chemistry or gas accretion.  Runs are deterministic
// for a given recipe, whatever the thread count.
//
// Units: kpc, km/s, 10^10 solar masses; times are reported in Myr.
#pragma once

#include "fft.hpp"
#include "worker_pool.hpp"

#include "shaders/abi.hpp"

#include <array>
#include <atomic>
#include <complex>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace helios::examples {

struct GalaxyRecipe {
    std::uint32_t seed = 1;
    std::uint32_t grid = 512; // active mesh cells per edge, a power of two
    double half_extent_kpc = 24.0;

    std::uint32_t disk_stars = 1'200'000;
    std::uint32_t gas_clouds = 800'000;
    std::uint32_t bulge_stars = 160'000;

    double disk_mass = 4.5; // 10^10 solar masses
    double disk_scale_kpc = 3.0;
    double disk_height_kpc = 0.3;
    double gas_mass = 0.8;
    double gas_scale_kpc = 5.0;
    double bulge_mass = 0.6;
    double bulge_scale_kpc = 0.45;
    double halo_speed_km_s = 180.0;
    double halo_core_kpc = 6.0;
    // Central black hole, 10^10 solar masses: Sagittarius A*, 4.30e6
    // (GRAVITY Collaboration 2022).
    double black_hole_mass = 4.30e-4;

    // Toomre's stability parameter of the stellar disk at 2.5 scale lengths.
    double toomre_q = 1.3;
    double softening_kpc = 0.2;
    double time_step_myr = 0.5;
    // Effective sound speed of the turbulent interstellar medium.
    double gas_sound_speed_km_s = 8.0;
    // Time over which random cloud motions dissipate.
    double gas_relaxation_myr = 8.0;
    // Momentum that supernovae and winds return to the gas per unit mass of
    // stars formed (Ostriker & Shetty 2011), delivered over the lifetime of
    // the stars that explode.  Zero switches feedback off.
    double feedback_momentum_km_s = 3000.0;
    double feedback_duration_myr = 30.0;

    [[nodiscard]] bool operator==(GalaxyRecipe const&) const = default;
};

struct GalaxySnapshot {
    double time_myr = 0.0;
    std::uint64_t step = 0;
    std::uint32_t grid = 0;
    std::uint32_t disk_stars = 0;
    std::uint32_t bulge_stars = 0;
    std::uint32_t clouds = 0;      // gas particles, including those that became stars
    std::uint32_t stars_formed = 0;
    double star_formation_rate = 0.0; // solar masses per year, last 10 Myr
    double step_ms = 0.0;             // wall time of the most recent step
    // Its parts: drift and mass deposit, field solve, kick and drift.
    double deposit_ms = 0.0;
    double mesh_ms = 0.0;
    double kick_ms = 0.0;

    // One record per particle, in the order disk, bulge, clouds: position,
    // velocity and birth time, packed as the GPU reads them.  The velocity
    // lets the display move the particles smoothly between snapshots.
    std::vector<shaders::GalaxyParticle> particles;
    // grid * grid texels of four floats: r = gas surface density, g = gas
    // scale height (kpc), b = stellar surface density, a = surface density
    // of stars younger than the feedback duration; surface densities in solar
    // masses per square parsec.
    std::vector<float> maps;
    double half_extent_kpc = 0.0; // of the maps

    // The star particles (not gas) sorted into a square grid in the plane
    // over +-kGalaxyPositionKpc: the indices of the particles in cell c are
    // cell_particles[cell_first[c] .. cell_first[c + 1]).
    static constexpr std::uint32_t kCells = 128;
    std::vector<std::uint32_t> cell_first;
    std::vector<std::uint32_t> cell_particles;
};

inline constexpr float kGasBirth = 1.0e30f;

// A snapshot record in physical units.
struct GalaxyParticleState {
    float position_kpc[3]{};
    float velocity_km_s[3]{};
    float birth_myr = 0.0f; // kGasBirth while still gas
};
[[nodiscard]] shaders::GalaxyParticle packParticle(GalaxyParticleState const& state) noexcept;
[[nodiscard]] GalaxyParticleState unpackParticle(shaders::GalaxyParticle const& particle) noexcept;

class GalaxySimulation {
  public:
    GalaxySimulation(GalaxyRecipe const& recipe, WorkerPool& pool);

    void step();

    [[nodiscard]] GalaxyRecipe const& recipe() const noexcept { return recipe_; }
    [[nodiscard]] double timeMyr() const noexcept;
    [[nodiscard]] std::uint64_t stepCount() const noexcept { return steps_; }
    // Solar masses per particle.
    [[nodiscard]] double diskParticleMass() const noexcept { return disk_particle_mass_ * 1.0e10; }
    [[nodiscard]] double bulgeParticleMass() const noexcept { return bulge_particle_mass_ * 1.0e10; }
    [[nodiscard]] double cloudParticleMass() const noexcept { return cloud_particle_mass_ * 1.0e10; }

    void snapshot(GalaxySnapshot& out) const;

    // Rotation curve measured from the forces actually used, for diagnostics:
    // circular speed (km/s) at `radius_kpc`.
    [[nodiscard]] double circularSpeed(double radius_kpc) const;

    // Heights at which the disk's field is tabulated: the softening length
    // times powers of two (0.2 to 6.4 kpc by default).
    static constexpr std::size_t kLayers = 6;

  private:
    struct RadialProfile {
        double step = 0.0;
        std::vector<double> speed2;  // circular speed squared
        std::vector<double> kappa2;  // epicyclic frequency squared
    };

    void initialise();
    void driftAndDeposit(float half_step);
    void solveMesh();
    void blur(std::vector<float> const& source, std::vector<float>& target, double sigma_cells);
    void kickAndDrift(float step, float half_step);
    [[nodiscard]] RadialProfile measureRotation() const;
    // Texels of one layer of `layers_`.
    [[nodiscard]] float const* layer(std::size_t index) const noexcept {
        return layers_.data() + index * static_cast<std::size_t>(n_) * n_ * 4U;
    }

    GalaxyRecipe recipe_;
    WorkerPool& pool_;
    Fft fft_;

    std::uint32_t n_ = 0;       // mesh cells per edge
    float cell_ = 0.0f;         // kpc
    float half_extent_ = 0.0f;  // kpc
    float dt_ = 0.0f;           // internal time units
    double time_ = 0.0;         // internal time units
    std::uint64_t steps_ = 0;

    std::uint32_t disk_count_ = 0;
    std::uint32_t bulge_count_ = 0;
    std::uint32_t cloud_count_ = 0;
    double disk_particle_mass_ = 0.0;
    double bulge_particle_mass_ = 0.0;
    double cloud_particle_mass_ = 0.0;

    // Particles, in the order disk, bulge, clouds.
    std::vector<float> x_, y_, z_, vx_, vy_, vz_, birth_;

    // Mesh fields, n * n each.
    std::vector<float> star_mass_, gas_mass_, gas_px_, gas_py_;
    std::vector<float> young_mass_;        // stars younger than the feedback duration
    std::vector<float> gas_smooth_;        // gas mass, smoothed over a cell
    // The disk's field at each tabulated effective height, four floats per
    // cell: in-plane acceleration (x, y), the vertical pull (2 pi G times the
    // surface density seen from that height), unused.
    std::vector<float> layers_;
    // The vertical pull at one disk scale height: sets the thickness of the
    // gas layer.
    std::vector<float> midplane_pull_;
    std::vector<float> pressure_;          // vertically integrated gas pressure
    std::vector<float> push_x_, push_y_;   // gas pressure
    std::vector<float> scratch_;
    // Per-chunk deposits, summed in a fixed order.
    std::vector<float> chunk_star_, chunk_gas_, chunk_young_;
    std::vector<std::complex<float>> work_;
    std::vector<std::complex<float>> mass_transform_;
    // Per layer, the transforms of the potential kernel (real part) and of
    // the vertical-pull kernel (imaginary part), both real and even, so that
    // one inverse transform yields both fields.
    std::vector<std::complex<float>> kernels_;

    RadialProfile rotation_; // of the initial state

    std::vector<std::uint32_t> formed_in_step_; // per chunk
    std::vector<std::uint32_t> formation_history_; // stars formed per step, ring buffer
    std::uint32_t stars_formed_ = 0;
    double last_step_ms_ = 0.0;
    std::array<double, 3> last_phase_ms_{};
};

// Owns a simulation and the thread that builds and advances it, and hands the
// newest state to the renderer without either side waiting for the other.
class GalaxyRunner {
  public:
    // Returns at once; the initial conditions are built on the runner's thread.
    GalaxyRunner(GalaxyRecipe const& recipe, std::uint32_t threads);
    ~GalaxyRunner();

    GalaxyRunner(GalaxyRunner const&) = delete;
    GalaxyRunner& operator=(GalaxyRunner const&) = delete;

    [[nodiscard]] GalaxyRecipe const& recipe() const noexcept { return recipe_; }
    // Solar masses per particle.
    [[nodiscard]] double diskParticleMass() const noexcept;
    [[nodiscard]] double bulgeParticleMass() const noexcept;
    [[nodiscard]] double cloudParticleMass() const noexcept;

    // The simulation advances while its time is below the target.
    void setTargetMyr(double time_myr);
    // Blocks until the target set above has been reached and published.
    void waitForTarget();

    // Hands over the newest state if it is newer than `version`: `snapshot`
    // and the runner's copy change places, so nothing is copied.  Returns
    // false when there is nothing new.
    [[nodiscard]] bool fetch(GalaxySnapshot& snapshot, std::uint64_t& version);

    // Seconds the initial conditions took; zero until they are ready.
    [[nodiscard]] double initialiseSeconds() const noexcept {
        return initialise_seconds_.load(std::memory_order_relaxed);
    }

  private:
    void run();

    GalaxyRecipe recipe_;
    WorkerPool pool_;
    std::atomic<double> initialise_seconds_{0.0};

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable reached_;
    GalaxySnapshot published_;
    double published_time_myr_ = 0.0;
    std::uint64_t version_ = 0;
    double target_myr_ = 0.0;
    bool at_target_ = false;
    bool stopping_ = false;
    std::thread thread_;
};

} // namespace helios::examples

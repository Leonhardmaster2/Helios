// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// The galaxy of the deep-sky lab: owns the N-body run and draws its newest
// state, one image per star particle (shaders/galaxy_stars.slang).
//
// The simulation runs on its own threads and publishes a state a few times a
// tenth of a second.  Three things keep that from showing as stutter:
//
//   - a state is uploaded a few megabytes per frame into a second buffer,
//     which replaces the drawn one when it is complete;
//   - every particle carries its velocity, and is drawn where it is at the
//     display time, not where it was when the state was published;
//   - the display clock advances with wall time, as fast as asked for but no
//     faster than the simulation delivers.
//
// Every particle is also the tens of thousands of stars it stands for
// (stellar_systems.hpp).  Near the camera the brightest of them are drawn one
// by one (shaders/galaxy_resolved_stars.slang) and the particle's own image
// keeps only the light of the rest; any of them can be picked, followed as
// the galaxy turns, and visited: the star, its planets and their moons are
// drawn by shaders/star_system.slang.
#pragma once

#include "fullscreen_pass.hpp"
#include "galaxy_simulation.hpp"
#include "stellar_population.hpp"
#include "stellar_systems.hpp"
#include "worker_pool.hpp"

#include "helios/blackhole/blackbody.hpp"
#include "helios/math/vec.hpp"
#include "shaders/abi.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace helios::examples {

struct DeepSkyCamera;

// What the camera orbits.
struct GalaxyFocus {
    enum class Kind : std::uint8_t {
        Centre,   // the galaxy's centre, where its black hole is
        Particle, // a star particle, followed as it moves
        Star,     // one star of a particle
        Planet,   // a planet of that star
        Moon,     // a moon of that planet
    };
    Kind kind = Kind::Centre;
    std::uint32_t particle = 0;
    std::uint32_t rank = 0;
    std::uint32_t planet = 0;
    std::uint32_t moon = 0;

    [[nodiscard]] bool operator==(GalaxyFocus const&) const = default;
    [[nodiscard]] bool hasStar() const noexcept { return kind >= Kind::Star; }
};

struct GalaxySettings {
    GalaxyRecipe recipe{};
    // Simulation time at which the run stops.
    double time_myr = 600.0;
    // Simulated time shown per second of wall time while the display clock is
    // behind `time_myr`; zero holds it.  Ignored when update() is asked to
    // wait: then the clock jumps to `time_myr`.
    double speed_myr_per_second = 0.0;
    // Changing this starts the run again from its initial conditions.
    std::uint32_t generation = 0;
    // Dust per unit gas, relative to the Milky Way's.
    float dust = 1.0f;
    // Light of the ionised gas, relative to the computed value.
    float ionised_gas = 1.0f;
    // Width of an old star particle's image, in mean particle spacings.
    float smoothing = 3.5f;

    // The camera orbits `focus`; while `transition` runs from 0 to 1 the
    // point it looks at moves from `previous_focus` to it.
    GalaxyFocus focus{};
    GalaxyFocus previous_focus{};
    float transition = 1.0f;
    // Single stars drawn at most; the faintest are left in their particles'
    // light when there are more.
    std::uint32_t star_budget = 1'500'000;
    // Draw the stars of particles near the camera one by one.
    bool resolve_stars = true;

    [[nodiscard]] bool operator==(GalaxySettings const&) const = default;
};

struct GalaxyStatus {
    double time_myr = 0.0;         // of the newest simulated state
    double display_time_myr = 0.0; // of the image
    std::uint64_t steps = 0;
    std::uint32_t particles = 0;
    std::uint32_t stars_formed = 0;
    std::uint32_t clouds = 0;
    double star_formation_rate = 0.0; // solar masses per year
    double step_ms = 0.0;
    double initialise_seconds = 0.0;
    bool ready = false; // the initial conditions exist
    // The view.
    std::uint32_t near_particles = 0; // whose stars are drawn one by one
    std::uint64_t resolved_stars = 0;
    double faintest_luminosity = 0.0; // of a resolved star at 1 pc, solar
    double pre_exposure = 1.0;
    double near_radius_kpc = 0.0;
};

// Kinds of star to look for.
enum class StarSearch : std::uint8_t {
    SunLike,     // a main-sequence star of about one solar mass with planets
    RingedGiant, // a star with a ringed gas giant
    LavaWorld,   // a star with a planet whose day side is molten
    Giant,       // a red giant
    WhiteDwarf,  // a white dwarf, with what is left of its planets
};

// A body of the visited system as the camera sees it.
struct GalaxyBody {
    math::Vec3d world_kpc{};
    double radius_kpc = 0.0;
    GalaxyFocus focus{};
};

class GalaxyScene {
  public:
    enum class Pass : std::uint8_t {
        YoungStars = 0, // full resolution
        IonisedGas = 1, // full resolution
        OldStars = 2,   // the smooth light, meant for a half-resolution target
    };

    // `frame_buffer` is the lab's dynamic frame-constant buffer; the
    // black-body table colours single stars.
    [[nodiscard]] bool initialize(Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                                  Diligent::IBuffer* frame_buffer, Diligent::TEXTURE_FORMAT target_format,
                                  Diligent::ITextureView* blackbody_view,
                                  blackhole::BlackbodyTable const& blackbody, std::string& error);

    // Starts the run (or restarts it when the recipe or the generation
    // changed), advances the display clock and continues uploading the newest
    // state.  With `wait` it blocks until the stop time has been simulated
    // and shows exactly that.  `changed` reports whether the next image
    // differs from the last.
    [[nodiscard]] bool update(GalaxySettings const& settings, std::uint32_t threads, bool wait,
                              bool& changed, std::string& error);

    // After update(): places the camera, given as an orbit about the origin,
    // about the focus instead; chooses the stars drawn one by one and the
    // exposure; uploads what the passes below read.  `display_exposure` is
    // the factor the display chain applies (2^EV times the base exposure).
    [[nodiscard]] bool prepareView(DeepSkyCamera& camera, GalaxySettings const& settings,
                                   std::uint32_t width, std::uint32_t height, double display_exposure,
                                   std::string& error);

    void fillFrame(shaders::DeepSkyFrame& frame, GalaxySettings const& settings) const;

    // Draws one pass into the bound render target.  `frame` (built for that
    // target's size) is rewritten into the frame buffer with the pass set.
    void draw(shaders::DeepSkyFrame const& frame, Pass pass);
    // The stars near the camera, one by one, into the bound target.
    void drawResolvedStars();
    // The visited system (star, planets, moons) over `target`.
    void drawSystem(Diligent::ITextureView* target, std::uint32_t width, std::uint32_t height);

    [[nodiscard]] GalaxyStatus const& status() const noexcept { return status_; }

    // What a click at (x, y) in [0, 1]^2 of the last image lands on: a body of
    // the visited system, a single star, or else the nearest star particle.
    [[nodiscard]] std::optional<GalaxyFocus> pick(double x, double y) const;
    // A star of a kind, in a disk particle near `radius_kpc` from the centre
    // (the first found, in index order).
    [[nodiscard]] std::optional<GalaxyFocus> findStar(double radius_kpc, StarSearch search) const;

    // The system of the focused star (after prepareView), if any.
    [[nodiscard]] StarSystem const* system() const noexcept { return system_ ? &*system_ : nullptr; }
    [[nodiscard]] ParticleStars const& systemParticle() const noexcept { return system_particle_; }
    // World position (kpc) of a focus at the display time; and the focus,
    // camera and bodies of the last view.
    [[nodiscard]] std::optional<math::Vec3d> focusPosition(GalaxyFocus const& focus) const;
    [[nodiscard]] math::Vec3d const& cameraWorld() const noexcept { return camera_world_; }
    [[nodiscard]] math::Vec3d const& viewCentre() const noexcept { return view_centre_; }
    [[nodiscard]] std::vector<GalaxyBody> const& bodies() const noexcept { return bodies_; }
    // Radius (kpc) of the focused body, zero for a point.
    [[nodiscard]] double focusRadius(GalaxyFocus const& focus) const;
    // Projects a world position (kpc) with the last view: x, y in [-1, 1]
    // (y up), false behind the camera.
    [[nodiscard]] bool project(math::Vec3d const& world_kpc, double& x, double& y) const;
    // Seconds of display time: a coarse and a fine part (their sum).
    [[nodiscard]] double displayCoarseSeconds() const noexcept;
    [[nodiscard]] double displayFineSeconds() const noexcept { return display_fine_s_; }
    // World position (kpc) of a planet of the visited system at a time
    // offset (s) from the display time, for drawing its orbit.
    [[nodiscard]] std::optional<math::Vec3d> planetWorld(std::uint32_t planet, double offset_s) const;

  private:
    struct NearRecord {
        math::Vec3d world_kpc{}; // centre at the display time
        ParticleStars stars{};
        float fade = 0.0f;
        float optical_depth = 0.0f;
        std::uint32_t slots = 0;
    };

    [[nodiscard]] bool ensureBuffers(std::string& error);
    // Uploads up to `budget` bytes of the incoming state; `finished` is set
    // when the state is complete and has become the drawn one.
    [[nodiscard]] bool continueUpload(std::size_t budget, bool& finished, std::string& error);
    [[nodiscard]] bool createStarPipelines(Diligent::TEXTURE_FORMAT target_format, std::string& error);
    [[nodiscard]] double displayMyr() const noexcept;
    [[nodiscard]] math::Vec3d particlePosition(std::uint32_t index) const;
    [[nodiscard]] double particleMass(std::uint32_t index) const;
    [[nodiscard]] double particleBirth(std::uint32_t index) const;
    [[nodiscard]] bool isBulge(std::uint32_t index) const noexcept;
    [[nodiscard]] ParticleStars starsOf(std::uint32_t index, math::Vec3d const& position) const;
    [[nodiscard]] float gasColumn(math::Vec3d const& position, math::Vec3d const& to_camera) const;
    void selectNear(GalaxySettings const& settings, double pixel_angle, double display_exposure);
    void buildSystemView(GalaxySettings const& settings);
    void adaptExposure(double target);
    [[nodiscard]] bool uploadView(std::string& error);

    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device_;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> context_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> frame_buffer_;
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> pipeline_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> resources_;
    Diligent::RefCntAutoPtr<Diligent::ISampler> sampler_;
    Diligent::RefCntAutoPtr<Diligent::ITexture> population_texture_;
    Diligent::RefCntAutoPtr<Diligent::ITexture> maps_texture_;
    // The drawn state and the one being filled.
    std::array<Diligent::RefCntAutoPtr<Diligent::IBuffer>, 2> particles_;
    std::size_t front_ = 0;
    bool has_front_ = false;
    PopulationTable population_{};
    math::Vec3d ionised_light_{};

    // Single stars and the visited system.
    Diligent::RefCntAutoPtr<Diligent::ITextureView> blackbody_view_;
    blackhole::BlackbodyTable blackbody_{};
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> star_pipeline_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> star_resources_;
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> system_pipeline_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> system_resources_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> system_buffer_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> near_buffer_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> chunk_buffer_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> unresolved_buffer_;
    std::size_t near_capacity_ = 0;
    std::size_t chunk_capacity_ = 0;
    std::vector<shaders::GalaxyNearParticle> near_upload_;
    std::vector<std::uint32_t> chunk_upload_; // record, first slot
    std::vector<std::uint8_t> unresolved_;    // per particle: light left to its image
    std::vector<std::uint32_t> unresolved_set_; // entries below 255
    bool unresolved_dirty_ = true;
    std::vector<NearRecord> near_;
    std::uint32_t star_instances_ = 0;
    ResolvedLightTable resolved_light_{};
    WorkerPool view_pool_{3};
    double budget_scale_ = 1.0; // raises the faintest drawn star while over budget
    double near_radius_kpc_ = 3.0;
    double solar_log_luminance_ = 0.0;
    double pre_exposure_ = 1.0;
    float sky_scale_ = 1.0f;
    // The exposure follows its target like an eye adapting, except in a
    // capture; wall time of the last update.
    double last_elapsed_ = 0.0;
    bool adapt_at_once_ = true;

    std::optional<StarSystem> system_{};
    ParticleStars system_particle_{};
    std::vector<GalaxyBody> bodies_{};
    shaders::StarSystemBlock system_block_{};
    bool draw_system_ = false;

    std::unique_ptr<GalaxyRunner> runner_;
    std::uint32_t generation_ = 0;
    std::uint64_t version_ = 0;
    GalaxySnapshot incoming_{}; // being uploaded, or the storage handed back next
    GalaxySnapshot front_state_{}; // the drawn state, on the CPU
    bool uploading_ = false;
    std::size_t uploaded_bytes_ = 0;
    std::uint32_t buffer_particles_ = 0;
    std::uint32_t maps_grid_ = 0;

    // The drawn state.
    double front_time_myr_ = 0.0;
    std::uint32_t front_particles_ = 0;
    std::uint32_t disk_stars_ = 0;
    std::uint32_t bulge_stars_ = 0;

    // Display clock: a coarse part in Myr and a fine part in seconds, so
    // that hours still count after billions of years.
    double display_time_myr_ = 0.0;
    double display_fine_s_ = 0.0;
    double rate_scale_ = 1.0;
    std::chrono::steady_clock::time_point last_update_{};
    GalaxyStatus status_{};

    // The last view.
    math::Vec3d camera_world_{};
    math::Vec3d view_centre_{};
    math::Vec3d camera_forward_{0.0, 1.0, 0.0};
    math::Vec3d camera_right_{1.0, 0.0, 0.0}; // scaled by tan(fov / 2) * aspect
    math::Vec3d camera_up_{0.0, 0.0, 1.0};    // scaled by tan(fov / 2)
    double pixel_angle_ = 1.0e-3;
};

} // namespace helios::examples

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "galaxy_simulation.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numbers>

namespace helios::examples {
namespace {

constexpr double kGravity = 43009.1;     // kpc (km/s)^2 per 10^10 solar masses
constexpr double kTimeUnitMyr = 977.792; // kpc / (km/s)
constexpr double kPi = std::numbers::pi;
// 10^10 solar masses per kpc^2 in solar masses per pc^2.
constexpr double kSolarPerPc2 = 1.0e4;

// Kennicutt (1998): the star formation rate per unit area is
// 2.5e-4 (Sigma_gas / Msun pc^-2)^1.4 Msun / yr / kpc^2, so gas of surface
// density Sigma turns into stars at the rate 2.5e-4 Sigma^0.4 per Myr.
constexpr double kSchmidtRatePerMyr = 2.5e-4;
constexpr double kSchmidtIndex = 0.4;
// Below a few solar masses per pc^2 disks form no stars (Kennicutt 1989).
constexpr double kFormationThreshold = 4.0;

// Particles are processed in a fixed number of chunks, each with its own
// deposit grid, so sums are taken in the same order on every machine.
constexpr std::size_t kDepositChunks = 8;
constexpr std::size_t kKickChunks = 96;

constexpr double kDiskAgeMyr = 10000.0;     // disk stars: constant formation over 10 Gyr
constexpr double kBulgeYoungestMyr = 9000.0; // bulge stars: 9 - 12 Gyr old
constexpr double kBulgeOldestMyr = 12000.0;
constexpr double kBulgeSoftening = 0.05;    // kpc
constexpr double kBulgeTruncation = 10.0;   // scale radii
// The black hole is softened like the bulge's core: the leapfrog step stays
// stable at the centre (omega dt < 0.3), and nothing smaller than this is
// resolved by the mesh anyway.
constexpr double kBlackHoleSoftening = kBulgeSoftening; // kpc
// Radius over which young stars push on the gas: the size of the superbubbles
// that clustered supernovae blow, a few hundred parsecs.
constexpr double kFeedbackReach = 0.25; // kpc
// Ratio of successive effective heights at which the disk's field is
// tabulated.
constexpr double kLayerRatio = 2.0;
constexpr float kLog2 = 0.69314718f;

// ---- deterministic random numbers -------------------------------------------

[[nodiscard]] std::uint64_t mix(std::uint64_t z) {
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// Uniform in (0, 1), a pure function of its arguments.
[[nodiscard]] double uniform(std::uint64_t seed, std::uint64_t index, std::uint64_t stream) {
    std::uint64_t const h = mix(mix(seed * 0xD1B54A32D192ED03ULL + stream) ^ (index * 0x8CB92BA72F3D8DD7ULL));
    return (static_cast<double>(h >> 11) + 0.5) * (1.0 / 9007199254740992.0);
}

[[nodiscard]] double gaussian(std::uint64_t seed, std::uint64_t index, std::uint64_t stream) {
    double const u1 = uniform(seed, index, stream);
    double const u2 = uniform(seed, index, stream + 1);
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
}

// Radius enclosing the fraction `u` of an exponential disk truncated at
// `r_max`: solves 1 - (1 + y) exp(-y) = u F(y_max) for y = R / scale.
[[nodiscard]] double exponentialRadius(double u, double scale, double r_max) {
    double const y_max = r_max / scale;
    double const target = u * (1.0 - (1.0 + y_max) * std::exp(-y_max));
    double low = 0.0;
    double high = y_max;
    for (int iteration = 0; iteration < 40; ++iteration) {
        double const middle = 0.5 * (low + high);
        if (1.0 - (1.0 + middle) * std::exp(-middle) < target) {
            low = middle;
        } else {
            high = middle;
        }
    }
    return 0.5 * (low + high) * scale;
}

// Mass fraction of an exponential disk inside radius r (in the plane; also
// used as the sphere it is replaced by for the bulge orbits).
[[nodiscard]] double exponentialEnclosed(double r, double scale) {
    double const y = r / scale;
    return 1.0 - (1.0 + y) * std::exp(-y);
}

struct Range {
    std::size_t begin = 0;
    std::size_t end = 0;
};

[[nodiscard]] Range chunkRange(std::size_t first, std::size_t count, std::size_t chunk,
                               std::size_t chunks) {
    return {first + count * chunk / chunks, first + count * (chunk + 1) / chunks};
}

struct Cell {
    std::size_t index = 0; // of the lower-left of the four cells
    float w00 = 0.0f;
    float w10 = 0.0f;
    float w01 = 0.0f;
    float w11 = 0.0f;
    bool inside = false;
};

// Cloud-in-cell weights of a point on a mesh of n x n cells of size 1/inverse
// centred on the origin.
[[nodiscard]] Cell locate(float x, float y, float half_extent, float inverse_cell, std::uint32_t n) {
    Cell cell;
    float const u = (x + half_extent) * inverse_cell - 0.5f;
    float const v = (y + half_extent) * inverse_cell - 0.5f;
    float const fu = std::floor(u);
    float const fv = std::floor(v);
    if (!(fu >= 0.0f && fv >= 0.0f && fu < static_cast<float>(n - 1U) && fv < static_cast<float>(n - 1U))) {
        return cell;
    }
    float const tx = u - fu;
    float const ty = v - fv;
    cell.index = static_cast<std::size_t>(fv) * n + static_cast<std::size_t>(fu);
    cell.w00 = (1.0f - tx) * (1.0f - ty);
    cell.w10 = tx * (1.0f - ty);
    cell.w01 = (1.0f - tx) * ty;
    cell.w11 = tx * ty;
    cell.inside = true;
    return cell;
}

[[nodiscard]] float gather(std::vector<float> const& field, Cell const& cell, std::uint32_t n) {
    float const* const p = field.data() + cell.index;
    return p[0] * cell.w00 + p[1] * cell.w10 + p[n] * cell.w01 + p[n + 1U] * cell.w11;
}

void scatter(float* field, Cell const& cell, std::uint32_t n, float value) {
    float* const p = field + cell.index;
    p[0] += value * cell.w00;
    p[1] += value * cell.w10;
    p[n] += value * cell.w01;
    p[n + 1U] += value * cell.w11;
}

// The disk's field from one layer: in-plane acceleration and vertical pull.
struct Pull {
    float x = 0.0f;
    float y = 0.0f;
    float vertical = 0.0f;
};

[[nodiscard]] Pull gatherPull(float const* layer, Cell const& cell, std::uint32_t n) {
    float const* const p00 = layer + cell.index * 4U;
    float const* const p10 = p00 + 4;
    float const* const p01 = p00 + static_cast<std::size_t>(n) * 4U;
    float const* const p11 = p01 + 4;
    return {p00[0] * cell.w00 + p10[0] * cell.w10 + p01[0] * cell.w01 + p11[0] * cell.w11,
            p00[1] * cell.w00 + p10[1] * cell.w10 + p01[1] * cell.w01 + p11[1] * cell.w11,
            p00[2] * cell.w00 + p10[2] * cell.w10 + p01[2] * cell.w01 + p11[2] * cell.w11};
}

// Effective height of a star at height z above a layer of thickness h:
// eps + h ln cosh(z / h), whose z-derivative is tanh(z / h).
[[nodiscard]] float effectiveHeight(float z, float softening, float thickness) {
    float const x = std::abs(z) / thickness;
    return softening + thickness * (x + std::log1p(std::exp(-2.0f * x)) - kLog2);
}

} // namespace

GalaxySimulation::GalaxySimulation(GalaxyRecipe const& recipe, WorkerPool& pool)
    : recipe_(recipe), pool_(pool), fft_(static_cast<std::size_t>(recipe.grid) * 2U) {
    n_ = recipe_.grid;
    half_extent_ = static_cast<float>(recipe_.half_extent_kpc);
    cell_ = 2.0f * half_extent_ / static_cast<float>(n_);
    dt_ = static_cast<float>(recipe_.time_step_myr / kTimeUnitMyr);
    disk_count_ = recipe_.disk_stars;
    bulge_count_ = recipe_.bulge_stars;
    cloud_count_ = recipe_.gas_clouds;
    disk_particle_mass_ = recipe_.disk_mass / std::max<double>(disk_count_, 1.0);
    bulge_particle_mass_ = recipe_.bulge_mass / std::max<double>(bulge_count_, 1.0);
    cloud_particle_mass_ = recipe_.gas_mass / std::max<double>(cloud_count_, 1.0);

    std::size_t const cells = static_cast<std::size_t>(n_) * n_;
    star_mass_.assign(cells, 0.0f);
    gas_mass_.assign(cells, 0.0f);
    gas_px_.assign(cells, 0.0f);
    gas_py_.assign(cells, 0.0f);
    young_mass_.assign(cells, 0.0f);
    gas_smooth_.assign(cells, 0.0f);
    layers_.assign(cells * kLayers * 4U, 0.0f);
    midplane_pull_.assign(cells, 0.0f);
    pressure_.assign(cells, 0.0f);
    push_x_.assign(cells, 0.0f);
    push_y_.assign(cells, 0.0f);
    scratch_.assign(cells, 0.0f);
    chunk_star_.assign(cells * kDepositChunks, 0.0f);
    chunk_gas_.assign(cells * kDepositChunks * 3U, 0.0f);
    chunk_young_.assign(cells * kDepositChunks, 0.0f);
    formed_in_step_.assign(kKickChunks, 0U);
    formation_history_.assign(
        static_cast<std::size_t>(std::max(std::lround(10.0 / recipe_.time_step_myr), 1L)), 0U);

    // Kernels on the doubled mesh (Hockney & Eastwood's isolated boundary
    // conditions; separations run from -n to n - 1 cells along each axis),
    // one pair per effective height a: the potential of a unit mass seen
    // from height a, -G / sqrt(r^2 + a^2), and its derivative with respect
    // to a, G a / (r^2 + a^2)^(3/2), the Poisson kernel times 2 pi G.  The
    // lowest height is the softening length, so the in-plane force is the
    // softened one.  Both kernels are real and even, so their transforms are
    // real: the transform of (potential + i pull) holds one in its real part
    // and the other in its imaginary part.
    std::size_t const m = static_cast<std::size_t>(n_) * 2U;
    work_.assign(m * m, {});
    mass_transform_.assign(m * m, {});
    kernels_.assign(kLayers * m * m, {});
    float const normalisation = 1.0f / static_cast<float>(m * m);
    for (std::size_t layer = 0; layer < kLayers; ++layer) {
        double const height = recipe_.softening_kpc * std::pow(kLayerRatio, static_cast<double>(layer));
        double const height2 = height * height;
        pool_.run(m, [&](std::size_t j) {
            double const dy = static_cast<double>(j < n_ ? j : m - j) * cell_;
            for (std::size_t i = 0; i < m; ++i) {
                double const dx = static_cast<double>(i < n_ ? i : m - i) * cell_;
                double const r2 = dx * dx + dy * dy + height2;
                double const r = std::sqrt(r2);
                work_[j * m + i] = {static_cast<float>(-kGravity / r),
                                    static_cast<float>(kGravity * height / (r2 * r))};
            }
        });
        transform2d(fft_, work_, false, pool_);
        std::complex<float>* const kernel = kernels_.data() + layer * m * m;
        pool_.run(m, [&](std::size_t j) {
            for (std::size_t index = j * m; index < (j + 1U) * m; ++index) {
                kernel[index] = work_[index] * normalisation;
            }
        });
    }

    initialise();
}

double GalaxySimulation::timeMyr() const noexcept {
    return time_ * kTimeUnitMyr;
}

namespace {

struct Potentials {
    double halo_speed2 = 0.0;
    double halo_core2 = 0.0;
    double bulge_gm = 0.0;
    double bulge_scale = 0.0;
    double hole_gm = 0.0;
    double disk_gm = 0.0;
    double disk_scale = 0.0;
    double gas_gm = 0.0;
    double gas_scale = 0.0;
    double softening2 = 0.0;

    explicit Potentials(GalaxyRecipe const& recipe)
        : halo_speed2(recipe.halo_speed_km_s * recipe.halo_speed_km_s),
          halo_core2(recipe.halo_core_kpc * recipe.halo_core_kpc),
          bulge_gm(kGravity * recipe.bulge_mass), bulge_scale(recipe.bulge_scale_kpc),
          hole_gm(kGravity * std::max(recipe.black_hole_mass, 0.0)),
          disk_gm(kGravity * recipe.disk_mass), disk_scale(recipe.disk_scale_kpc),
          gas_gm(kGravity * recipe.gas_mass), gas_scale(recipe.gas_scale_kpc),
          softening2(recipe.softening_kpc * recipe.softening_kpc) {}

    // Acceleration towards the centre divided by the radius, from the halo,
    // the bulge (a softened Hernquist sphere) and the black hole (a softened
    // point mass), at radius^2 = r2.
    [[nodiscard]] double spheroid(double r2) const {
        double const rs = std::sqrt(r2 + kBulgeSoftening * kBulgeSoftening);
        double const sum = rs + bulge_scale;
        double const hole2 = r2 + kBlackHoleSoftening * kBlackHoleSoftening;
        return halo_speed2 / (r2 + halo_core2) + bulge_gm / (rs * sum * sum) +
               hole_gm / (hole2 * std::sqrt(hole2));
    }

    // The same for the disk and gas replaced by spheres of equal enclosed
    // mass: what the bulge orbits feel, and what the few particles that leave
    // the mesh feel.
    [[nodiscard]] double diskSphere(double r2) const {
        double const r = std::sqrt(r2);
        double const enclosed = disk_gm * exponentialEnclosed(r, disk_scale) +
                                gas_gm * exponentialEnclosed(r, gas_scale);
        double const soft = r2 + softening2;
        return enclosed / (soft * std::sqrt(soft));
    }
};

} // namespace

void GalaxySimulation::initialise() {
    std::size_t const total =
        static_cast<std::size_t>(disk_count_) + bulge_count_ + cloud_count_;
    for (auto* array : {&x_, &y_, &z_, &vx_, &vy_, &vz_, &birth_}) {
        array->assign(total, 0.0f);
    }
    std::uint64_t const seed = recipe_.seed;
    Potentials const potentials{recipe_};
    double const r_max = std::min(6.5 * recipe_.disk_scale_kpc, 0.92 * recipe_.half_extent_kpc);
    std::size_t const bulge_first = disk_count_;
    std::size_t const cloud_first = bulge_first + bulge_count_;

    // ---- positions --------------------------------------------------------
    pool_.run(kKickChunks, [&](std::size_t chunk) {
        Range const disk = chunkRange(0, disk_count_, chunk, kKickChunks);
        for (std::size_t i = disk.begin; i < disk.end; ++i) {
            double const radius = exponentialRadius(uniform(seed, i, 1), recipe_.disk_scale_kpc, r_max);
            double const angle = 2.0 * kPi * uniform(seed, i, 2);
            x_[i] = static_cast<float>(radius * std::cos(angle));
            y_[i] = static_cast<float>(radius * std::sin(angle));
            // Isothermal sheet: density proportional to sech^2(z / z0).
            double const u = std::clamp(uniform(seed, i, 3), 1.0e-6, 1.0 - 1.0e-6);
            z_[i] = static_cast<float>(recipe_.disk_height_kpc * std::atanh(2.0 * u - 1.0));
            birth_[i] = static_cast<float>(-kDiskAgeMyr * uniform(seed, i, 4));
        }
        Range const clouds = chunkRange(cloud_first, cloud_count_, chunk, kKickChunks);
        for (std::size_t i = clouds.begin; i < clouds.end; ++i) {
            double const radius = exponentialRadius(uniform(seed, i, 1), recipe_.gas_scale_kpc, r_max);
            double const angle = 2.0 * kPi * uniform(seed, i, 2);
            x_[i] = static_cast<float>(radius * std::cos(angle));
            y_[i] = static_cast<float>(radius * std::sin(angle));
            birth_[i] = kGasBirth;
        }
        Range const bulge = chunkRange(bulge_first, bulge_count_, chunk, kKickChunks);
        for (std::size_t i = bulge.begin; i < bulge.end; ++i) {
            // Hernquist (1990): M(<r) / M = r^2 / (r + a)^2.
            double const limit = kBulgeTruncation / (kBulgeTruncation + 1.0);
            double const s = limit * std::sqrt(uniform(seed, i, 1));
            double const radius = recipe_.bulge_scale_kpc * s / (1.0 - s);
            double const cosine = 2.0 * uniform(seed, i, 2) - 1.0;
            double const sine = std::sqrt(std::max(1.0 - cosine * cosine, 0.0));
            double const angle = 2.0 * kPi * uniform(seed, i, 3);
            x_[i] = static_cast<float>(radius * sine * std::cos(angle));
            y_[i] = static_cast<float>(radius * sine * std::sin(angle));
            z_[i] = static_cast<float>(radius * cosine);
            birth_[i] = static_cast<float>(
                -(kBulgeYoungestMyr + (kBulgeOldestMyr - kBulgeYoungestMyr) * uniform(seed, i, 4)));
        }
    });

    // ---- forces of this particular realisation ------------------------------
    driftAndDeposit(0.0f);
    solveMesh();
    rotation_ = measureRotation();
    RadialProfile const& rotation = rotation_;
    auto profileAt = [&](std::vector<double> const& values, double radius) {
        double const position = std::clamp(radius / rotation.step - 0.5, 0.0,
                                           static_cast<double>(values.size()) - 1.001);
        auto const index = static_cast<std::size_t>(position);
        double const t = position - static_cast<double>(index);
        return values[index] * (1.0 - t) + values[index + 1] * t;
    };

    // ---- disk kinematics (Hernquist 1993) -----------------------------------
    double const disk_sigma0 = recipe_.disk_mass /
                               (2.0 * kPi * recipe_.disk_scale_kpc * recipe_.disk_scale_kpc *
                                exponentialEnclosed(r_max, recipe_.disk_scale_kpc));
    double const gas_sigma0 = recipe_.gas_mass /
                              (2.0 * kPi * recipe_.gas_scale_kpc * recipe_.gas_scale_kpc *
                               exponentialEnclosed(r_max, recipe_.gas_scale_kpc));
    double const reference = 2.5 * recipe_.disk_scale_kpc;
    double const reference_kappa = std::sqrt(profileAt(rotation.kappa2, reference));
    // Radial dispersion: exp(-R / 2 R_d), normalised to Toomre's Q at the
    // reference radius (Q = sigma_R kappa / 3.36 G Sigma).
    double const radial_sigma0 = recipe_.toomre_q * 3.36 * kGravity * disk_sigma0 *
                                 std::exp(-reference / recipe_.disk_scale_kpc) / reference_kappa *
                                 std::exp(reference / (2.0 * recipe_.disk_scale_kpc));
    double const sound = recipe_.gas_sound_speed_km_s;

    // ---- bulge kinematics: isotropic Jeans equation -------------------------
    // The bulge is truncated where its particles were placed, so the
    // pressure integral runs from that radius inwards (the dispersion falls
    // to zero at the edge), and no star is given more energy than it takes to
    // climb to the edge: an untruncated Hernquist dispersion with Gaussian
    // tails puts a few per cent of the bulge on orbits that reach tens of
    // kiloparsecs.
    constexpr std::size_t kJeansSamples = 384;
    double const jeans_inner = 0.005 * recipe_.bulge_scale_kpc;
    double const jeans_outer = kBulgeTruncation * recipe_.bulge_scale_kpc;
    double const jeans_step = std::log(jeans_outer / jeans_inner) / (kJeansSamples - 1);
    std::vector<double> bulge_sigma2(kJeansSamples, 0.0);
    // Squared speed that carries a star from r to the edge.
    std::vector<double> bulge_reach2(kJeansSamples, 0.0);
    {
        auto density = [&](double r) {
            double const sum = r + recipe_.bulge_scale_kpc;
            return 1.0 / (r * sum * sum * sum);
        };
        auto pull = [&](double r) {
            return (potentials.spheroid(r * r) + potentials.diskSphere(r * r)) * r;
        };
        double integral = 0.0;
        double climb = 0.0;
        double previous = 0.0;
        double previous_pull = 0.0;
        for (std::size_t index = kJeansSamples; index-- > 0;) {
            double const r = jeans_inner * std::exp(jeans_step * static_cast<double>(index));
            // Integrands in ln r.
            double const integrand = density(r) * pull(r) * r;
            double const pull_r = pull(r) * r;
            if (index + 1 < kJeansSamples) {
                integral += 0.5 * (integrand + previous) * jeans_step;
                climb += 0.5 * (pull_r + previous_pull) * jeans_step;
            }
            previous = integrand;
            previous_pull = pull_r;
            bulge_sigma2[index] = integral / density(r);
            bulge_reach2[index] = 2.0 * climb;
        }
    }

    pool_.run(kKickChunks, [&](std::size_t chunk) {
        Range const disk = chunkRange(0, disk_count_, chunk, kKickChunks);
        for (std::size_t i = disk.begin; i < disk.end; ++i) {
            double const radius = std::max(static_cast<double>(std::hypot(x_[i], y_[i])), 1.0e-3);
            double const speed2 = profileAt(rotation.speed2, radius);
            double const kappa2 = profileAt(rotation.kappa2, radius);
            double const omega2 = std::max(speed2 / (radius * radius), 1.0e-9);
            double const ratio = std::clamp(kappa2 / (4.0 * omega2), 0.25, 1.0);
            double const sigma_r = radial_sigma0 * std::exp(-radius / (2.0 * recipe_.disk_scale_kpc));
            double const sigma_phi = sigma_r * std::sqrt(ratio);
            double const surface = disk_sigma0 * std::exp(-radius / recipe_.disk_scale_kpc) +
                                   gas_sigma0 * std::exp(-radius / recipe_.gas_scale_kpc);
            double const sigma_z = std::sqrt(kPi * kGravity * surface * recipe_.disk_height_kpc);
            // Asymmetric drift from the radial Jeans equation.
            double const mean2 =
                speed2 + sigma_r * sigma_r * (1.0 - ratio - 2.0 * radius / recipe_.disk_scale_kpc);
            double const mean = std::sqrt(std::max(mean2, 0.0));
            double const v_r = sigma_r * gaussian(seed, i, 10);
            double const v_phi = mean + sigma_phi * gaussian(seed, i, 12);
            double const cosine = x_[i] / radius;
            double const sine = y_[i] / radius;
            vx_[i] = static_cast<float>(v_r * cosine - v_phi * sine);
            vy_[i] = static_cast<float>(v_r * sine + v_phi * cosine);
            vz_[i] = static_cast<float>(sigma_z * gaussian(seed, i, 14));
        }
        Range const clouds = chunkRange(cloud_first, cloud_count_, chunk, kKickChunks);
        for (std::size_t i = clouds.begin; i < clouds.end; ++i) {
            double const radius = std::max(static_cast<double>(std::hypot(x_[i], y_[i])), 1.0e-3);
            // Rotation balances gravity less the outward pressure gradient.
            double const speed2 =
                profileAt(rotation.speed2, radius) - sound * sound * radius / recipe_.gas_scale_kpc;
            double const v_phi = std::sqrt(std::max(speed2, 0.0)) + 0.3 * sound * gaussian(seed, i, 12);
            double const v_r = 0.3 * sound * gaussian(seed, i, 10);
            double const cosine = x_[i] / radius;
            double const sine = y_[i] / radius;
            vx_[i] = static_cast<float>(v_r * cosine - v_phi * sine);
            vy_[i] = static_cast<float>(v_r * sine + v_phi * cosine);
        }
        Range const bulge = chunkRange(bulge_first, bulge_count_, chunk, kKickChunks);
        for (std::size_t i = bulge.begin; i < bulge.end; ++i) {
            double const radius = std::sqrt(static_cast<double>(x_[i]) * x_[i] +
                                            static_cast<double>(y_[i]) * y_[i] +
                                            static_cast<double>(z_[i]) * z_[i]);
            double const position = std::clamp(std::log(std::max(radius, jeans_inner) / jeans_inner) /
                                                   jeans_step,
                                               0.0, static_cast<double>(kJeansSamples) - 1.001);
            auto const index = static_cast<std::size_t>(position);
            double const t = position - static_cast<double>(index);
            double const sigma =
                std::sqrt(bulge_sigma2[index] * (1.0 - t) + bulge_sigma2[index + 1] * t);
            double const reach2 = bulge_reach2[index] * (1.0 - t) + bulge_reach2[index + 1] * t;
            // Bound to the bulge: velocities that would leave it are drawn
            // again (a truncated Gaussian), and the rare star still too fast
            // after that is slowed to just below the limit.
            double v[3]{};
            for (std::uint64_t attempt = 0; attempt < 16; ++attempt) {
                std::uint64_t const stream = 10 + attempt * 6;
                v[0] = sigma * gaussian(seed, i, stream);
                v[1] = sigma * gaussian(seed, i, stream + 2);
                v[2] = sigma * gaussian(seed, i, stream + 4);
                if (v[0] * v[0] + v[1] * v[1] + v[2] * v[2] <= reach2) {
                    break;
                }
            }
            double const speed2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
            double const scale = speed2 > reach2 ? 0.95 * std::sqrt(reach2 / speed2) : 1.0;
            vx_[i] = static_cast<float>(v[0] * scale);
            vy_[i] = static_cast<float>(v[1] * scale);
            vz_[i] = static_cast<float>(v[2] * scale);
        }
    });
}

GalaxySimulation::RadialProfile GalaxySimulation::measureRotation() const {
    RadialProfile profile;
    profile.step = cell_;
    std::size_t const bins = n_ / 2U;
    profile.speed2.assign(bins, 0.0);
    profile.kappa2.assign(bins, 0.0);
    Potentials const potentials{recipe_};
    float const inverse_cell = 1.0f / cell_;
    constexpr int kAzimuths = 128;
    for (std::size_t bin = 0; bin < bins; ++bin) {
        double const radius = (static_cast<double>(bin) + 0.5) * profile.step;
        double inward = 0.0;
        int samples = 0;
        for (int k = 0; k < kAzimuths; ++k) {
            double const angle = 2.0 * kPi * (static_cast<double>(k) + 0.5) / kAzimuths;
            double const cosine = std::cos(angle);
            double const sine = std::sin(angle);
            Cell const cell = locate(static_cast<float>(radius * cosine),
                                     static_cast<float>(radius * sine), half_extent_, inverse_cell, n_);
            if (!cell.inside) {
                continue;
            }
            // In the plane: the lowest layer, at the softening length.
            Pull const pull = gatherPull(layer(0), cell, n_);
            inward -= static_cast<double>(pull.x) * cosine + static_cast<double>(pull.y) * sine;
            ++samples;
        }
        double const mesh = samples > 0 ? inward / samples : potentials.diskSphere(radius * radius) * radius;
        profile.speed2[bin] =
            std::max(radius * (mesh + potentials.spheroid(radius * radius) * radius), 0.0);
    }
    // Two passes of a 1-2-1 filter: the epicyclic frequency needs a derivative.
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<double> const source = profile.speed2;
        for (std::size_t bin = 1; bin + 1 < bins; ++bin) {
            profile.speed2[bin] = 0.25 * source[bin - 1] + 0.5 * source[bin] + 0.25 * source[bin + 1];
        }
    }
    for (std::size_t bin = 0; bin < bins; ++bin) {
        double const radius = (static_cast<double>(bin) + 0.5) * profile.step;
        std::size_t const lower = bin > 0 ? bin - 1 : bin;
        std::size_t const upper = bin + 1 < bins ? bin + 1 : bin;
        double const slope = (profile.speed2[upper] - profile.speed2[lower]) /
                             (static_cast<double>(upper - lower) * profile.step);
        double const omega2 = profile.speed2[bin] / (radius * radius);
        // kappa^2 = R d(Omega^2)/dR + 4 Omega^2, between Keplerian and rigid.
        profile.kappa2[bin] = std::clamp(2.0 * omega2 + slope / radius, omega2, 4.0 * omega2);
    }
    return profile;
}

double GalaxySimulation::circularSpeed(double radius_kpc) const {
    if (rotation_.speed2.empty()) {
        return 0.0;
    }
    double const position = std::clamp(radius_kpc / rotation_.step - 0.5, 0.0,
                                       static_cast<double>(rotation_.speed2.size()) - 1.001);
    auto const index = static_cast<std::size_t>(position);
    double const t = position - static_cast<double>(index);
    return std::sqrt(rotation_.speed2[index] * (1.0 - t) + rotation_.speed2[index + 1] * t);
}

void GalaxySimulation::driftAndDeposit(float half_step) {
    std::size_t const cells = static_cast<std::size_t>(n_) * n_;
    float const inverse_cell = 1.0f / cell_;
    auto const disk_mass = static_cast<float>(disk_particle_mass_);
    auto const cloud_mass = static_cast<float>(cloud_particle_mass_);
    std::size_t const bulge_first = disk_count_;
    std::size_t const cloud_first = bulge_first + bulge_count_;
    // Stars born after this time still drive feedback.
    auto const young_since = static_cast<float>(timeMyr() - recipe_.feedback_duration_myr);

    pool_.run(kDepositChunks, [&](std::size_t chunk) {
        float* const stars = chunk_star_.data() + chunk * cells;
        float* const young = chunk_young_.data() + chunk * cells;
        std::memset(young, 0, cells * sizeof(float));
        float* const gas = chunk_gas_.data() + chunk * cells * 3U;
        float* const gas_px = gas + cells;
        float* const gas_py = gas + 2U * cells;
        std::memset(stars, 0, cells * sizeof(float));
        std::memset(gas, 0, cells * 3U * sizeof(float));

        Range const disk = chunkRange(0, disk_count_, chunk, kDepositChunks);
        for (std::size_t i = disk.begin; i < disk.end; ++i) {
            x_[i] += vx_[i] * half_step;
            y_[i] += vy_[i] * half_step;
            z_[i] += vz_[i] * half_step;
            Cell const cell = locate(x_[i], y_[i], half_extent_, inverse_cell, n_);
            if (cell.inside) {
                scatter(stars, cell, n_, disk_mass);
            }
        }
        Range const bulge = chunkRange(bulge_first, bulge_count_, chunk, kDepositChunks);
        for (std::size_t i = bulge.begin; i < bulge.end; ++i) {
            x_[i] += vx_[i] * half_step;
            y_[i] += vy_[i] * half_step;
            z_[i] += vz_[i] * half_step;
        }
        Range const clouds = chunkRange(cloud_first, cloud_count_, chunk, kDepositChunks);
        for (std::size_t i = clouds.begin; i < clouds.end; ++i) {
            x_[i] += vx_[i] * half_step;
            y_[i] += vy_[i] * half_step;
            z_[i] += vz_[i] * half_step;
            Cell const cell = locate(x_[i], y_[i], half_extent_, inverse_cell, n_);
            if (!cell.inside) {
                continue;
            }
            if (birth_[i] == kGasBirth) {
                scatter(gas, cell, n_, cloud_mass);
                scatter(gas_px, cell, n_, cloud_mass * vx_[i]);
                scatter(gas_py, cell, n_, cloud_mass * vy_[i]);
            } else {
                scatter(stars, cell, n_, cloud_mass);
                if (birth_[i] > young_since) {
                    scatter(young, cell, n_, cloud_mass);
                }
            }
        }
    });

    pool_.run(n_, [&](std::size_t row) {
        std::size_t const first = row * n_;
        for (std::size_t index = first; index < first + n_; ++index) {
            float stars = 0.0f;
            float young = 0.0f;
            float gas = 0.0f;
            float px = 0.0f;
            float py = 0.0f;
            for (std::size_t chunk = 0; chunk < kDepositChunks; ++chunk) {
                stars += chunk_star_[chunk * cells + index];
                young += chunk_young_[chunk * cells + index];
                float const* const g = chunk_gas_.data() + chunk * cells * 3U;
                gas += g[index];
                px += g[cells + index];
                py += g[2U * cells + index];
            }
            star_mass_[index] = stars;
            young_mass_[index] = young;
            gas_mass_[index] = gas;
            gas_px_[index] = px;
            gas_py_[index] = py;
        }
    });
}

void GalaxySimulation::solveMesh() {
    std::size_t const n = n_;
    std::size_t const m = n * 2U;

    // Transform of the mass.
    pool_.run(m, [&](std::size_t row) {
        std::complex<float>* const out = work_.data() + row * m;
        std::fill(out, out + m, std::complex<float>{});
        if (row < n) {
            for (std::size_t i = 0; i < n; ++i) {
                out[i] = star_mass_[row * n + i] + gas_mass_[row * n + i];
            }
        }
    });
    transform2dPadded(fft_, work_, false, n, pool_);
    std::copy(work_.begin(), work_.end(), mass_transform_.begin());

    // At each effective height, one inverse transform gives the potential
    // (real part) and the vertical pull (imaginary part).  The in-plane
    // acceleration is the potential's centred difference; rows and columns -1
    // and n are valid on the doubled mesh, so the edges need no special case.
    float const scale = -0.5f / cell_;
    for (std::size_t index = 0; index < kLayers; ++index) {
        std::complex<float> const* const kernel = kernels_.data() + index * m * m;
        pool_.run(m, [&](std::size_t row) {
            for (std::size_t cell = row * m; cell < (row + 1U) * m; ++cell) {
                work_[cell] = mass_transform_[cell] * kernel[cell];
            }
        });
        // Rows 0 .. n and the last row (row -1) are read below.
        transform2dPadded(fft_, work_, true, n + 1U, pool_);
        float* const out = layers_.data() + index * n * n * 4U;
        pool_.run(n, [&](std::size_t j) {
            std::size_t const below = (j + m - 1U) % m;
            std::size_t const above = j + 1U;
            for (std::size_t i = 0; i < n; ++i) {
                std::size_t const left = (i + m - 1U) % m;
                std::size_t const right = i + 1U;
                float* const texel = out + (j * n + i) * 4U;
                texel[0] = scale * (work_[j * m + right].real() - work_[j * m + left].real());
                texel[1] = scale * (work_[above * m + i].real() - work_[below * m + i].real());
                texel[2] = std::max(work_[j * m + i].imag(), 0.0f);
                texel[3] = 0.0f;
            }
        });
    }

    // The pull at one disk scale height above the plane, which holds the
    // gas layer together.
    {
        double const level = std::log(std::max(effectiveHeight(static_cast<float>(recipe_.disk_height_kpc),
                                                                static_cast<float>(recipe_.softening_kpc),
                                                                static_cast<float>(recipe_.disk_height_kpc)) /
                                                    recipe_.softening_kpc,
                                                1.0)) /
                             std::log(kLayerRatio);
        auto const lower = std::min(static_cast<std::size_t>(level), kLayers - 2U);
        auto const t = static_cast<float>(std::min(level - static_cast<double>(lower), 1.0));
        float const* const a = layer(lower);
        float const* const b = layer(lower + 1U);
        pool_.run(n, [&](std::size_t j) {
            for (std::size_t index = j * n; index < (j + 1U) * n; ++index) {
                midplane_pull_[index] = a[index * 4U + 2U] * (1.0f - t) + b[index * 4U + 2U] * t;
            }
        });
    }

    // The gas pressure is a gradient and would otherwise follow the particle
    // noise.
    blur(gas_mass_, gas_smooth_, 1.0);
    // Young stars act on the gas over the size of the bubbles they blow.
    blur(young_mass_, pressure_, kFeedbackReach / cell_);

    // Vertically integrated pressure: the turbulent medium, c^2 Sigma, plus
    // the momentum flux of feedback through the gas layer,
    // (h / 2) (p / m) Sigma_SFR, with the star formation rate taken from the
    // stars younger than the feedback duration.
    //
    // The sound speed has a floor that keeps the Jeans length of the gas,
    // c^2 / (G Sigma), above the softening length: collapse on smaller scales
    // cannot be followed by this mesh and would otherwise end in a few
    // artificially massive lumps (cf. Truelove et al. 1997).
    auto const sound = static_cast<float>(recipe_.gas_sound_speed_km_s);
    float const sound2 = sound * sound;
    auto const two_pi_g = static_cast<float>(2.0 * kPi * kGravity);
    auto const disk_height = static_cast<float>(recipe_.disk_height_kpc);
    float const inverse_area = 1.0f / (cell_ * cell_);
    auto const feedback = static_cast<float>(
        0.5 * recipe_.feedback_momentum_km_s / (recipe_.feedback_duration_myr / kTimeUnitMyr));
    auto const jeans = static_cast<float>(kPi * kGravity * recipe_.softening_kpc) * inverse_area;
    float const least_pull = two_pi_g * 1.0e-6f;
    pool_.run(n, [&](std::size_t j) {
        for (std::size_t index = j * n; index < (j + 1U) * n; ++index) {
            float const height = std::clamp(
                sound * std::sqrt(disk_height / std::max(midplane_pull_[index], least_pull)), 0.02f, 0.5f);
            float const support = std::max(sound2, jeans * gas_smooth_[index]);
            pressure_[index] = support * gas_smooth_[index] + feedback * height * pressure_[index];
        }
    });

    // a = -grad(P) / Sigma.  The floor of two particles per cell switches the
    // force off where the gas is too sparse to have a meaningful gradient.
    auto const floor = static_cast<float>(2.0 * cloud_particle_mass_);
    float const push = -0.5f / cell_;
    pool_.run(n, [&](std::size_t j) {
        std::size_t const below = j > 0 ? j - 1U : j;
        std::size_t const above = j + 1U < n ? j + 1U : j;
        for (std::size_t i = 0; i < n; ++i) {
            std::size_t const left = i > 0 ? i - 1U : i;
            std::size_t const right = i + 1U < n ? i + 1U : i;
            float const inverse = push / (gas_smooth_[j * n + i] + floor);
            push_x_[j * n + i] = inverse * (pressure_[j * n + right] - pressure_[j * n + left]);
            push_y_[j * n + i] = inverse * (pressure_[above * n + i] - pressure_[below * n + i]);
        }
    });
}

// Separable Gaussian filter with clamped edges.
void GalaxySimulation::blur(std::vector<float> const& source, std::vector<float>& target,
                            double sigma_cells) {
    std::size_t const n = n_;
    int const radius = std::max(static_cast<int>(std::ceil(3.0 * sigma_cells)), 1);
    std::vector<float> taps(static_cast<std::size_t>(radius) * 2U + 1U);
    double sum = 0.0;
    for (int k = -radius; k <= radius; ++k) {
        double const value = std::exp(-0.5 * k * k / (sigma_cells * sigma_cells));
        taps[static_cast<std::size_t>(k + radius)] = static_cast<float>(value);
        sum += value;
    }
    for (float& tap : taps) {
        tap = static_cast<float>(tap / sum);
    }
    auto const last = static_cast<int>(n) - 1;
    pool_.run(n, [&](std::size_t j) {
        for (std::size_t i = 0; i < n; ++i) {
            float value = 0.0f;
            for (int k = -radius; k <= radius; ++k) {
                int const column = std::clamp(static_cast<int>(i) + k, 0, last);
                value += taps[static_cast<std::size_t>(k + radius)] *
                         source[j * n + static_cast<std::size_t>(column)];
            }
            scratch_[j * n + i] = value;
        }
    });
    pool_.run(n, [&](std::size_t j) {
        for (std::size_t i = 0; i < n; ++i) {
            float value = 0.0f;
            for (int k = -radius; k <= radius; ++k) {
                int const row = std::clamp(static_cast<int>(j) + k, 0, last);
                value += taps[static_cast<std::size_t>(k + radius)] *
                         scratch_[static_cast<std::size_t>(row) * n + i];
            }
            target[j * n + i] = value;
        }
    });
}

void GalaxySimulation::kickAndDrift(float step, float half_step) {
    Potentials const potentials{recipe_};
    float const inverse_cell = 1.0f / cell_;
    float const inverse_area = 1.0f / (cell_ * cell_);
    auto const two_pi_g = static_cast<float>(2.0 * kPi * kGravity);
    auto const disk_height = static_cast<float>(recipe_.disk_height_kpc);
    auto const sound = static_cast<float>(recipe_.gas_sound_speed_km_s);
    auto const relaxation =
        static_cast<float>(1.0 - std::exp(-recipe_.time_step_myr / recipe_.gas_relaxation_myr));
    double const end_time_myr = (time_ + static_cast<double>(step)) * kTimeUnitMyr;
    std::uint64_t const seed = recipe_.seed;
    std::size_t const bulge_first = disk_count_;
    std::size_t const cloud_first = bulge_first + bulge_count_;

    // A star: the disk's field at the star's effective height (in-plane and
    // vertical, both from one potential), and the spheroidal components.
    float const inverse_log_ratio = 1.0f / std::log(static_cast<float>(kLayerRatio));
    auto const softening = static_cast<float>(recipe_.softening_kpc);
    float const top_level = static_cast<float>(kLayers - 1U);
    float const top_height = softening * std::pow(static_cast<float>(kLayerRatio), top_level);

    // Orbits through the stiff centre (the bulge's softened core and the
    // black hole), or across it in less than a step: there the leapfrog's
    // error is large and plunging orbits gain energy, so the step is split,
    // with the mesh's pull held fixed and the analytic central pull
    // recomputed at every sub-step.  On entry the star has drifted half a
    // step; on exit it sits half a step of drift before the end, as after an
    // ordinary kick.  Returns false when one step suffices.
    constexpr float kSubStepLimit = 0.3f;
    constexpr int kMaxSubSteps = 32;
    auto const core = static_cast<float>(kBulgeSoftening);
    auto finishStep = [&](std::size_t i, float ax, float ay, float pull, float central, bool sphere) {
        float const radius = std::sqrt(x_[i] * x_[i] + y_[i] * y_[i] + z_[i] * z_[i]);
        float const speed = std::sqrt(vx_[i] * vx_[i] + vy_[i] * vy_[i] + vz_[i] * vz_[i]);
        float const stiffness = std::max(std::sqrt(std::max(central, 0.0f)) * step,
                                         speed * step / std::max(radius, core));
        if (stiffness <= kSubStepLimit) {
            return false;
        }
        int const count = std::min(static_cast<int>(std::ceil(stiffness / kSubStepLimit)), kMaxSubSteps);
        float const sub = step / static_cast<float>(count);
        float const half_sub = 0.5f * sub;
        x_[i] -= vx_[i] * half_step;
        y_[i] -= vy_[i] * half_step;
        z_[i] -= vz_[i] * half_step;
        for (int s = 0; s < count; ++s) {
            x_[i] += vx_[i] * half_sub;
            y_[i] += vy_[i] * half_sub;
            z_[i] += vz_[i] * half_sub;
            double const r2 = static_cast<double>(x_[i]) * x_[i] + static_cast<double>(y_[i]) * y_[i] +
                              static_cast<double>(z_[i]) * z_[i];
            auto const pull_in =
                static_cast<float>(potentials.spheroid(r2) + (sphere ? potentials.diskSphere(r2) : 0.0));
            float const az = -pull * std::tanh(z_[i] / disk_height);
            vx_[i] += (ax - pull_in * x_[i]) * sub;
            vy_[i] += (ay - pull_in * y_[i]) * sub;
            vz_[i] += (az - pull_in * z_[i]) * sub;
            x_[i] += vx_[i] * half_sub;
            y_[i] += vy_[i] * half_sub;
            z_[i] += vz_[i] * half_sub;
        }
        x_[i] -= vx_[i] * half_step;
        y_[i] -= vy_[i] * half_step;
        z_[i] -= vz_[i] * half_step;
        return true;
    };

    auto moveStar = [&](std::size_t i) {
        double const r2 = static_cast<double>(x_[i]) * x_[i] + static_cast<double>(y_[i]) * y_[i] +
                          static_cast<double>(z_[i]) * z_[i];
        auto central = static_cast<float>(potentials.spheroid(r2));
        float ax = 0.0f;
        float ay = 0.0f;
        float az = 0.0f;
        Cell const cell = locate(x_[i], y_[i], half_extent_, inverse_cell, n_);
        if (cell.inside) {
            float const height = effectiveHeight(z_[i], softening, disk_height);
            float level = std::log(height / softening) * inverse_log_ratio;
            // Above the highest layer the disk pulls like a point mass at
            // the centre: the top layer's field, scaled as -G M / d^2.
            float in_plane = 1.0f;
            float vertical = 1.0f;
            if (level > top_level) {
                float const cylinder2 = x_[i] * x_[i] + y_[i] * y_[i];
                float const ratio = (cylinder2 + top_height * top_height) / (cylinder2 + height * height);
                in_plane = ratio * std::sqrt(ratio);
                vertical = in_plane * height / top_height;
                level = top_level;
            }
            auto const lower = std::min(static_cast<std::size_t>(level), kLayers - 2U);
            float const t = level - static_cast<float>(lower);
            Pull const below = gatherPull(layer(lower), cell, n_);
            Pull const above = gatherPull(layer(lower + 1U), cell, n_);
            ax = (below.x + (above.x - below.x) * t) * in_plane;
            ay = (below.y + (above.y - below.y) * t) * in_plane;
            // d(effective height)/dz = tanh(z / h): inside the layer the pull
            // grows with the mass below the star, as in an isothermal sheet.
            float const pull = (below.vertical + (above.vertical - below.vertical) * t) * vertical;
            az = -pull * std::tanh(z_[i] / disk_height);
            if (!finishStep(i, ax, ay, pull, central, false)) {
                vx_[i] += (ax - central * x_[i]) * step;
                vy_[i] += (ay - central * y_[i]) * step;
                vz_[i] += (az - central * z_[i]) * step;
            }
        } else {
            central += static_cast<float>(potentials.diskSphere(r2));
            if (!finishStep(i, 0.0f, 0.0f, 0.0f, central, true)) {
                vx_[i] -= central * x_[i] * step;
                vy_[i] -= central * y_[i] * step;
                vz_[i] -= central * z_[i] * step;
            }
        }
        x_[i] += vx_[i] * half_step;
        y_[i] += vy_[i] * half_step;
        z_[i] += vz_[i] * half_step;
    };

    pool_.run(kKickChunks, [&](std::size_t chunk) {
        Range const disk = chunkRange(0, disk_count_, chunk, kKickChunks);
        for (std::size_t i = disk.begin; i < disk.end; ++i) {
            moveStar(i);
        }

        Range const bulge = chunkRange(bulge_first, bulge_count_, chunk, kKickChunks);
        for (std::size_t i = bulge.begin; i < bulge.end; ++i) {
            double const r2 = static_cast<double>(x_[i]) * x_[i] +
                              static_cast<double>(y_[i]) * y_[i] +
                              static_cast<double>(z_[i]) * z_[i];
            auto const central =
                static_cast<float>(potentials.spheroid(r2) + potentials.diskSphere(r2));
            if (!finishStep(i, 0.0f, 0.0f, 0.0f, central, true)) {
                vx_[i] -= central * x_[i] * step;
                vy_[i] -= central * y_[i] * step;
                vz_[i] -= central * z_[i] * step;
            }
            x_[i] += vx_[i] * half_step;
            y_[i] += vy_[i] * half_step;
            z_[i] += vz_[i] * half_step;
        }

        std::uint32_t formed = 0;
        Range const clouds = chunkRange(cloud_first, cloud_count_, chunk, kKickChunks);
        for (std::size_t i = clouds.begin; i < clouds.end; ++i) {
            if (birth_[i] != kGasBirth) {
                moveStar(i);
                continue;
            }
            double const r2 =
                static_cast<double>(x_[i]) * x_[i] + static_cast<double>(y_[i]) * y_[i];
            auto central = static_cast<float>(potentials.spheroid(r2));
            Cell const cell = locate(x_[i], y_[i], half_extent_, inverse_cell, n_);
            if (!cell.inside) {
                central += static_cast<float>(potentials.diskSphere(r2));
                vx_[i] -= central * x_[i] * step;
                vy_[i] -= central * y_[i] * step;
                x_[i] += vx_[i] * half_step;
                y_[i] += vy_[i] * half_step;
                continue;
            }
            // Gas stays in the plane: the lowest layer.
            Pull const gravity = gatherPull(layer(0), cell, n_);
            float const ax = gravity.x + gather(push_x_, cell, n_);
            float const ay = gravity.y + gather(push_y_, cell, n_);
            vx_[i] += (ax - central * x_[i]) * step;
            vy_[i] += (ay - central * y_[i]) * step;

            // Clouds collide inelastically: random motion relative to the
            // local mean flow decays.
            float const local_mass = gather(gas_mass_, cell, n_);
            if (local_mass > 0.0f) {
                float const mean_x = gather(gas_px_, cell, n_) / local_mass;
                float const mean_y = gather(gas_py_, cell, n_) / local_mass;
                vx_[i] += relaxation * (mean_x - vx_[i]);
                vy_[i] += relaxation * (mean_y - vy_[i]);
            }

            // Star formation at the Kennicutt-Schmidt rate.
            double const gas_surface =
                static_cast<double>(gather(gas_smooth_, cell, n_) * inverse_area) * kSolarPerPc2;
            if (gas_surface > kFormationThreshold) {
                double const probability =
                    -std::expm1(-kSchmidtRatePerMyr * std::pow(gas_surface, kSchmidtIndex) *
                                recipe_.time_step_myr);
                if (uniform(seed, i, 1000 + steps_) < probability) {
                    // The new stars start in the gas layer, whose thickness
                    // follows from hydrostatic balance in the disk's pull.
                    float const pull = gather(midplane_pull_, cell, n_);
                    float const height = std::clamp(
                        sound * std::sqrt(disk_height / std::max(pull, two_pi_g * 1.0e-6f)), 0.02f,
                        0.5f);
                    birth_[i] = static_cast<float>(end_time_myr);
                    z_[i] = height * static_cast<float>(gaussian(seed, i, 20));
                    vz_[i] = sound * static_cast<float>(gaussian(seed, i, 22));
                    ++formed;
                }
            }
            x_[i] += vx_[i] * half_step;
            y_[i] += vy_[i] * half_step;
        }
        formed_in_step_[chunk] = formed;
    });
}

void GalaxySimulation::step() {
    using Clock = std::chrono::steady_clock;
    auto const begin = Clock::now();
    float const half_step = 0.5f * dt_;
    driftAndDeposit(half_step);
    auto const deposited = Clock::now();
    solveMesh();
    auto const solved = Clock::now();
    kickAndDrift(dt_, half_step);
    auto const kicked = Clock::now();
    time_ += static_cast<double>(dt_);
    last_phase_ms_[0] = std::chrono::duration<double, std::milli>(deposited - begin).count();
    last_phase_ms_[1] = std::chrono::duration<double, std::milli>(solved - deposited).count();
    last_phase_ms_[2] = std::chrono::duration<double, std::milli>(kicked - solved).count();

    std::uint32_t formed = 0;
    for (std::uint32_t count : formed_in_step_) {
        formed += count;
    }
    stars_formed_ += formed;
    formation_history_[steps_ % formation_history_.size()] = formed;
    ++steps_;
    last_step_ms_ =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

void GalaxySimulation::snapshot(GalaxySnapshot& out) const {
    out.time_myr = timeMyr();
    out.step = steps_;
    out.grid = n_;
    out.disk_stars = disk_count_;
    out.bulge_stars = bulge_count_;
    out.clouds = cloud_count_;
    out.stars_formed = stars_formed_;
    out.step_ms = last_step_ms_;
    out.deposit_ms = last_phase_ms_[0];
    out.mesh_ms = last_phase_ms_[1];
    out.kick_ms = last_phase_ms_[2];
    std::uint64_t recent = 0;
    for (std::uint32_t count : formation_history_) {
        recent += count;
    }
    double const window_years =
        static_cast<double>(formation_history_.size()) * recipe_.time_step_myr * 1.0e6;
    out.star_formation_rate = static_cast<double>(recent) * cloudParticleMass() / window_years;

    std::size_t const total = x_.size();
    out.particles.resize(total);
    pool_.run(kKickChunks, [&](std::size_t chunk) {
        Range const range = chunkRange(0, total, chunk, kKickChunks);
        for (std::size_t i = range.begin; i < range.end; ++i) {
            out.particles[i] = packParticle(
                {{x_[i], y_[i], z_[i]}, {vx_[i], vy_[i], vz_[i]}, birth_[i]});
        }
    });

    // Star particles by cell: a counting sort, in a fixed number of chunks so
    // that the order within a cell does not depend on the thread count.
    {
        constexpr std::size_t kGridChunks = 16;
        constexpr std::uint32_t kCells = GalaxySnapshot::kCells;
        constexpr std::size_t kCellCount = static_cast<std::size_t>(kCells) * kCells;
        float const scale = static_cast<float>(kCells) / (2.0f * shaders::kGalaxyPositionKpc);
        auto cellOf = [&](std::size_t i) -> std::uint32_t {
            auto const cx = static_cast<std::uint32_t>(std::clamp(
                (x_[i] + shaders::kGalaxyPositionKpc) * scale, 0.0f, static_cast<float>(kCells - 1U)));
            auto const cy = static_cast<std::uint32_t>(std::clamp(
                (y_[i] + shaders::kGalaxyPositionKpc) * scale, 0.0f, static_cast<float>(kCells - 1U)));
            return cy * kCells + cx;
        };
        std::vector<std::uint32_t> counts(kGridChunks * kCellCount, 0U);
        pool_.run(kGridChunks, [&](std::size_t chunk) {
            std::uint32_t* const count = counts.data() + chunk * kCellCount;
            Range const range = chunkRange(0, total, chunk, kGridChunks);
            for (std::size_t i = range.begin; i < range.end; ++i) {
                if (birth_[i] != kGasBirth) {
                    ++count[cellOf(i)];
                }
            }
        });
        // Offsets: cell-major, then chunk.
        out.cell_first.assign(kCellCount + 1U, 0U);
        std::uint32_t running = 0;
        for (std::size_t cell = 0; cell < kCellCount; ++cell) {
            out.cell_first[cell] = running;
            for (std::size_t chunk = 0; chunk < kGridChunks; ++chunk) {
                std::uint32_t const count = counts[chunk * kCellCount + cell];
                counts[chunk * kCellCount + cell] = running;
                running += count;
            }
        }
        out.cell_first[kCellCount] = running;
        out.cell_particles.resize(running);
        pool_.run(kGridChunks, [&](std::size_t chunk) {
            std::uint32_t* const offset = counts.data() + chunk * kCellCount;
            Range const range = chunkRange(0, total, chunk, kGridChunks);
            for (std::size_t i = range.begin; i < range.end; ++i) {
                if (birth_[i] != kGasBirth) {
                    out.cell_particles[offset[cellOf(i)]++] = static_cast<std::uint32_t>(i);
                }
            }
        });
    }

    std::size_t const cells = static_cast<std::size_t>(n_) * n_;
    out.maps.resize(cells * 4U);
    out.half_extent_kpc = half_extent_;
    auto const to_surface = static_cast<float>(kSolarPerPc2 / (static_cast<double>(cell_) * cell_));
    auto const least_pull = static_cast<float>(2.0 * kPi * kGravity * 1.0e-6);
    auto const disk_height = static_cast<float>(recipe_.disk_height_kpc);
    auto const sound = static_cast<float>(recipe_.gas_sound_speed_km_s);
    pool_.run(n_, [&](std::size_t row) {
        for (std::size_t index = row * n_; index < (row + 1U) * n_; ++index) {
            float* const texel = out.maps.data() + index * 4U;
            texel[0] = gas_smooth_[index] * to_surface;
            // Hydrostatic thickness of an isothermal gas layer in the disk.
            texel[1] = std::clamp(
                sound * std::sqrt(disk_height / std::max(midplane_pull_[index], least_pull)), 0.02f, 0.5f);
            texel[2] = star_mass_[index] * to_surface;
            texel[3] = young_mass_[index] * to_surface;
        }
    });
}

// ---- snapshot records --------------------------------------------------------

shaders::GalaxyParticle packParticle(GalaxyParticleState const& state) noexcept {
    shaders::GalaxyParticle particle{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        float const position =
            std::clamp(state.position_kpc[axis] / shaders::kGalaxyPositionKpc, -1.0f, 1.0f);
        particle.position[axis] =
            static_cast<std::uint16_t>(std::lround((position * 0.5f + 0.5f) * 65535.0f));
        float const velocity =
            std::clamp(state.velocity_km_s[axis] / shaders::kGalaxyVelocityKmS, -1.0f, 1.0f);
        particle.velocity[axis] = static_cast<std::int16_t>(std::lround(velocity * 32767.0f));
    }
    float const split = shaders::kGalaxyBirthSplit;
    float const recent_codes = 65534.0f - split - 1.0f;
    if (state.birth_myr >= kGasBirth) {
        particle.birth = shaders::kGalaxyBirthGas;
    } else if (state.birth_myr < shaders::kGalaxyBirthRecentMyr) {
        float const t = (state.birth_myr - shaders::kGalaxyBirthOldestMyr) /
                        (shaders::kGalaxyBirthRecentMyr - shaders::kGalaxyBirthOldestMyr);
        particle.birth = static_cast<std::uint16_t>(std::lround(std::clamp(t, 0.0f, 1.0f) * split));
    } else {
        float const t = (state.birth_myr - shaders::kGalaxyBirthRecentMyr) /
                        (shaders::kGalaxyBirthLatestMyr - shaders::kGalaxyBirthRecentMyr);
        particle.birth = static_cast<std::uint16_t>(
            split + 1.0f + std::round(std::clamp(t, 0.0f, 1.0f) * recent_codes));
    }
    return particle;
}

GalaxyParticleState unpackParticle(shaders::GalaxyParticle const& particle) noexcept {
    GalaxyParticleState state;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        state.position_kpc[axis] =
            (static_cast<float>(particle.position[axis]) / 65535.0f * 2.0f - 1.0f) *
            shaders::kGalaxyPositionKpc;
        state.velocity_km_s[axis] =
            static_cast<float>(particle.velocity[axis]) / 32767.0f * shaders::kGalaxyVelocityKmS;
    }
    float const split = shaders::kGalaxyBirthSplit;
    float const code = particle.birth;
    if (particle.birth == shaders::kGalaxyBirthGas) {
        state.birth_myr = kGasBirth;
    } else if (code <= split) {
        state.birth_myr = shaders::kGalaxyBirthOldestMyr +
                          (shaders::kGalaxyBirthRecentMyr - shaders::kGalaxyBirthOldestMyr) * code / split;
    } else {
        state.birth_myr = shaders::kGalaxyBirthRecentMyr +
                          (shaders::kGalaxyBirthLatestMyr - shaders::kGalaxyBirthRecentMyr) *
                              (code - split - 1.0f) / (65534.0f - split - 1.0f);
    }
    return state;
}

// ---- runner -----------------------------------------------------------------

GalaxyRunner::GalaxyRunner(GalaxyRecipe const& recipe, std::uint32_t threads)
    // Two cores are left to the thread that draws and to the graphics driver.
    : recipe_(recipe), pool_(std::max(threads, 3U) - 2U, true) {
    thread_ = std::thread([this] { run(); });
}

GalaxyRunner::~GalaxyRunner() {
    {
        std::scoped_lock lock{mutex_};
        stopping_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

double GalaxyRunner::diskParticleMass() const noexcept {
    return recipe_.disk_mass * 1.0e10 / std::max<double>(recipe_.disk_stars, 1.0);
}

double GalaxyRunner::bulgeParticleMass() const noexcept {
    return recipe_.bulge_mass * 1.0e10 / std::max<double>(recipe_.bulge_stars, 1.0);
}

double GalaxyRunner::cloudParticleMass() const noexcept {
    return recipe_.gas_mass * 1.0e10 / std::max<double>(recipe_.gas_clouds, 1.0);
}

void GalaxyRunner::setTargetMyr(double time_myr) {
    {
        std::scoped_lock lock{mutex_};
        target_myr_ = time_myr;
        at_target_ = version_ > 0 && published_time_myr_ >= target_myr_ - 1.0e-6;
    }
    wake_.notify_all();
}

void GalaxyRunner::waitForTarget() {
    std::unique_lock lock{mutex_};
    reached_.wait(lock, [this] { return at_target_; });
}

bool GalaxyRunner::fetch(GalaxySnapshot& snapshot, std::uint64_t& version) {
    std::scoped_lock lock{mutex_};
    if (version == version_) {
        return false;
    }
    std::swap(snapshot, published_);
    version = version_;
    return true;
}

void GalaxyRunner::run() {
    using Clock = std::chrono::steady_clock;
    lowerCurrentThreadPriority();
    Clock::time_point const begin = Clock::now();
    GalaxySimulation simulation{recipe_, pool_};
    initialise_seconds_.store(std::chrono::duration<double>(Clock::now() - begin).count(),
                              std::memory_order_relaxed);

    GalaxySnapshot scratch;
    Clock::time_point last_publish = Clock::now();
    auto publish = [&] {
        simulation.snapshot(scratch);
        double const time = scratch.time_myr;
        {
            std::scoped_lock lock{mutex_};
            // `scratch` receives whatever the renderer handed back: storage
            // to reuse, not a state.
            std::swap(published_, scratch);
            published_time_myr_ = time;
            ++version_;
            at_target_ = published_time_myr_ >= target_myr_ - 1.0e-6;
        }
        reached_.notify_all();
        last_publish = Clock::now();
    };
    publish();

    for (;;) {
        double target = 0.0;
        {
            std::unique_lock lock{mutex_};
            wake_.wait(lock, [&] {
                return stopping_ || simulation.timeMyr() < target_myr_ - 1.0e-6;
            });
            if (stopping_) {
                return;
            }
            target = target_myr_;
        }
        simulation.step();
        bool const reached = simulation.timeMyr() >= target - 1.0e-6;
        // Publishing packs every particle; do it at display rate at most.
        if (reached || Clock::now() - last_publish > std::chrono::milliseconds{33}) {
            publish();
        }
    }
}

} // namespace helios::examples

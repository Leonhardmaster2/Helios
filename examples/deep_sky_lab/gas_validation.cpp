// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "gas_validation.hpp"

#include "diligent_headless.hpp"
#include "hydro_simulation.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <vector>

namespace helios::examples {
namespace {

using Json = nlohmann::ordered_json;

// Toro's pressure function for one side of the Riemann problem and its
// derivative.
struct PressureFunction {
    double value = 0.0;
    double slope = 0.0;
};

[[nodiscard]] PressureFunction pressureFunction(double p, GasState const& side, double gamma) {
    double const sound = std::sqrt(gamma * side.pressure / side.density);
    if (p > side.pressure) {
        // Shock.
        double const a = 2.0 / ((gamma + 1.0) * side.density);
        double const b = (gamma - 1.0) / (gamma + 1.0) * side.pressure;
        double const root = std::sqrt(a / (p + b));
        return {(p - side.pressure) * root, root * (1.0 - 0.5 * (p - side.pressure) / (p + b))};
    }
    // Rarefaction.
    double const ratio = p / side.pressure;
    return {2.0 * sound / (gamma - 1.0) * (std::pow(ratio, (gamma - 1.0) / (2.0 * gamma)) - 1.0),
            std::pow(ratio, -(gamma + 1.0) / (2.0 * gamma)) / (side.density * sound)};
}

} // namespace

GasState riemannExact(GasState const& left, GasState const& right, double gamma, double xi) {
    double const cl = std::sqrt(gamma * left.pressure / left.density);
    double const cr = std::sqrt(gamma * right.pressure / right.density);
    // Pressure between the waves, by Newton's method.
    double p = std::max(0.5 * (left.pressure + right.pressure), 1.0e-12);
    for (int iteration = 0; iteration < 60; ++iteration) {
        PressureFunction const fl = pressureFunction(p, left, gamma);
        PressureFunction const fr = pressureFunction(p, right, gamma);
        double const next =
            std::max(p - (fl.value + fr.value + right.velocity - left.velocity) / (fl.slope + fr.slope),
                     1.0e-12);
        bool const converged = std::abs(next - p) < 1.0e-13 * (next + p);
        p = next;
        if (converged) {
            break;
        }
    }
    double const u = 0.5 * (left.velocity + right.velocity) +
                     0.5 * (pressureFunction(p, right, gamma).value -
                            pressureFunction(p, left, gamma).value);
    double const g = (gamma - 1.0) / (gamma + 1.0);

    if (xi <= u) {
        // Left of the contact.
        if (p > left.pressure) {
            double const ratio = p / left.pressure;
            double const shock = left.velocity -
                                 cl * std::sqrt((gamma + 1.0) / (2.0 * gamma) * ratio +
                                                (gamma - 1.0) / (2.0 * gamma));
            if (xi <= shock) {
                return left;
            }
            return {left.density * (ratio + g) / (g * ratio + 1.0), u, p};
        }
        double const head = left.velocity - cl;
        double const star_sound = cl * std::pow(p / left.pressure, (gamma - 1.0) / (2.0 * gamma));
        double const tail = u - star_sound;
        if (xi <= head) {
            return left;
        }
        if (xi >= tail) {
            return {left.density * std::pow(p / left.pressure, 1.0 / gamma), u, p};
        }
        double const factor = 2.0 / (gamma + 1.0) + g / cl * (left.velocity - xi);
        return {left.density * std::pow(factor, 2.0 / (gamma - 1.0)),
                2.0 / (gamma + 1.0) * (cl + 0.5 * (gamma - 1.0) * left.velocity + xi),
                left.pressure * std::pow(factor, 2.0 * gamma / (gamma - 1.0))};
    }
    // Right of the contact.
    if (p > right.pressure) {
        double const ratio = p / right.pressure;
        double const shock = right.velocity +
                             cr * std::sqrt((gamma + 1.0) / (2.0 * gamma) * ratio +
                                            (gamma - 1.0) / (2.0 * gamma));
        if (xi >= shock) {
            return right;
        }
        return {right.density * (ratio + g) / (g * ratio + 1.0), u, p};
    }
    double const head = right.velocity + cr;
    double const star_sound = cr * std::pow(p / right.pressure, (gamma - 1.0) / (2.0 * gamma));
    double const tail = u + star_sound;
    if (xi >= head) {
        return right;
    }
    if (xi <= tail) {
        return {right.density * std::pow(p / right.pressure, 1.0 / gamma), u, p};
    }
    double const factor = 2.0 / (gamma + 1.0) - g / cr * (right.velocity - xi);
    return {right.density * std::pow(factor, 2.0 / (gamma - 1.0)),
            2.0 / (gamma + 1.0) * (-cr + 0.5 * (gamma - 1.0) * right.velocity + xi),
            right.pressure * std::pow(factor, 2.0 * gamma / (gamma - 1.0))};
}

int runGasValidation(std::uint32_t resolution, std::string const& report_path,
                     bool validation_layers) {
    std::string error;
    auto device = DiligentHeadless::create(validation_layers, error);
    HydroSimulation simulation;
    if (device == nullptr || !simulation.initialize(device->device(), device->context(), error)) {
        std::fprintf(stderr, "GasValidation: %s\n", error.c_str());
        return 1;
    }
    std::size_t const n = resolution;
    double const cell = 1.0 / static_cast<double>(n);
    std::vector<float> a;
    std::vector<float> b;
    bool passed = true;
    Json report;
    report["adapter"] = device->adapterName();
    report["cells_per_edge"] = resolution;
    report["scheme"] = "MUSCL (van Leer) + HLLC/HLL, two-stage Runge-Kutta, float32";

    auto finish = [&](double until) {
        while (simulation.time() < until) {
            if (!simulation.advance(32, until, 0.0, true, error)) {
                return false;
            }
        }
        return simulation.measure(error) && simulation.readState(a, b, error);
    };

    // ---- Sod's shock tube -------------------------------------------------
    {
        constexpr double kGamma = 1.4;
        constexpr double kTime = 0.2;
        GasState const left{1.0, 0.0, 1.0};
        GasState const right{0.125, 0.0, 0.1};
        HydroSetup setup;
        setup.scenario = HydroScenario::ShockTube;
        setup.resolution = resolution;
        setup.half_extent = 0.5;
        setup.gamma = kGamma;
        setup.source[0] = {1.0f, 1.0f, 0.125f, 0.1f};
        if (!simulation.start(setup, 1, error) || !finish(kTime)) {
            std::fprintf(stderr, "GasValidation: %s\n", error.c_str());
            return 1;
        }
        // Mean absolute error along the tube, at the middle of the box.
        double density_error = 0.0;
        double velocity_error = 0.0;
        double pressure_error = 0.0;
        std::size_t const row = (n / 2U * n + n / 2U) * n;
        for (std::size_t i = 0; i < n; ++i) {
            double const x = (static_cast<double>(i) + 0.5) * cell - 0.5;
            GasState const exact = riemannExact(left, right, kGamma, x / kTime);
            float const* const ua = a.data() + (row + i) * 4U;
            float const* const ub = b.data() + (row + i) * 4U;
            double const density = ua[0];
            double const velocity = ua[1] / density;
            double const pressure = (kGamma - 1.0) * (ub[0] - 0.5 * density * velocity * velocity);
            density_error += std::abs(density - exact.density);
            velocity_error += std::abs(velocity - exact.velocity);
            pressure_error += std::abs(pressure - exact.pressure);
        }
        density_error /= static_cast<double>(n);
        velocity_error /= static_cast<double>(n);
        pressure_error /= static_cast<double>(n);
        // A second-order scheme at 128 cells resolves the smooth parts to a
        // few 1e-3; the discontinuities, a few cells wide, dominate the error.
        double const scale = 128.0 / static_cast<double>(n);
        bool const ok = density_error < 0.008 * scale && velocity_error < 0.015 * scale &&
                        pressure_error < 0.006 * scale;
        passed = passed && ok;
        std::printf("Shock tube (t = %.2f, %llu steps): mean |error| density %.5f, velocity %.5f, "
                    "pressure %.5f  %s\n",
                    kTime, static_cast<unsigned long long>(simulation.steps()), density_error,
                    velocity_error, pressure_error, ok ? "PASS" : "FAIL");
        report["shock_tube"] = {{"time", kTime},
                                {"steps", simulation.steps()},
                                {"mean_abs_error_density", density_error},
                                {"mean_abs_error_velocity", velocity_error},
                                {"mean_abs_error_pressure", pressure_error},
                                {"passed", ok}};
    }

    // ---- Sedov-Taylor blast -------------------------------------------------
    {
        constexpr double kGamma = 5.0 / 3.0;
        // Similarity constant of the spherical blast for gamma = 5/3 (Sedov 1959).
        constexpr double kSedov = 1.15167;
        constexpr double kTime = 0.05;
        constexpr double kEnergy = 1.0;
        HydroSetup setup;
        setup.scenario = HydroScenario::Blast;
        setup.resolution = resolution;
        setup.half_extent = 0.5;
        setup.gamma = kGamma;
        setup.source[0] = {static_cast<float>(kEnergy), static_cast<float>(3.0 * cell), 1.0f, 1.0e-5f};
        if (!simulation.start(setup, 1, error)) {
            std::fprintf(stderr, "GasValidation: %s\n", error.c_str());
            return 1;
        }
        HydroTotals const initial = simulation.totals();
        if (!finish(kTime)) {
            std::fprintf(stderr, "GasValidation: %s\n", error.c_str());
            return 1;
        }
        HydroTotals const final_totals = simulation.totals();

        // Shell-averaged density; the shock is where it falls through half of
        // the jump, outside the peak.
        std::vector<double> sum(n, 0.0);
        std::vector<double> count(n, 0.0);
        for (std::size_t z = 0; z < n; ++z) {
            for (std::size_t y = 0; y < n; ++y) {
                for (std::size_t x = 0; x < n; ++x) {
                    double const px = (static_cast<double>(x) + 0.5) * cell - 0.5;
                    double const py = (static_cast<double>(y) + 0.5) * cell - 0.5;
                    double const pz = (static_cast<double>(z) + 0.5) * cell - 0.5;
                    auto const shell = static_cast<std::size_t>(std::sqrt(px * px + py * py + pz * pz) / cell);
                    if (shell < n) {
                        sum[shell] += a[((z * n + y) * n + x) * 4U];
                        count[shell] += 1.0;
                    }
                }
            }
        }
        std::size_t peak = 0;
        double peak_density = 0.0;
        for (std::size_t shell = 0; shell < n / 2U; ++shell) {
            double const density = count[shell] > 0.0 ? sum[shell] / count[shell] : 0.0;
            if (density > peak_density) {
                peak_density = density;
                peak = shell;
            }
        }
        double const half = 0.5 * (peak_density + 1.0);
        double radius = 0.0;
        for (std::size_t shell = peak; shell + 1 < n / 2U; ++shell) {
            double const inner = sum[shell] / count[shell];
            double const outer = sum[shell + 1] / count[shell + 1];
            if (inner >= half && outer < half) {
                radius = (static_cast<double>(shell) + 0.5 + (inner - half) / (inner - outer)) * cell;
                break;
            }
        }
        // The deposit is a few cells across, so the energy on the mesh is not
        // exactly the nominal one; the blast knows only the former.
        double const expected = kSedov * std::pow(initial.energy * kTime * kTime, 0.2);
        double const radius_error = radius / expected - 1.0;
        double const mass_error = final_totals.mass / initial.mass - 1.0;
        double const energy_error = final_totals.energy / initial.energy - 1.0;
        bool const ok = std::abs(radius_error) < 0.03 && std::abs(mass_error) < 1.0e-4 &&
                        std::abs(energy_error) < 5.0e-3 && peak_density > 2.2;
        passed = passed && ok;
        std::printf("Blast wave (t = %.3f, %llu steps): shock radius %.4f, exact %.4f (%+.2f %%); "
                    "peak density %.2f; mass %+.1e, energy %+.1e  %s\n",
                    kTime, static_cast<unsigned long long>(simulation.steps()), radius, expected,
                    100.0 * radius_error, peak_density, mass_error, energy_error, ok ? "PASS" : "FAIL");
        report["blast_wave"] = {{"time", kTime},
                                {"steps", simulation.steps()},
                                {"shock_radius", radius},
                                {"shock_radius_exact", expected},
                                {"shock_radius_relative_error", radius_error},
                                {"peak_shell_density", peak_density},
                                {"mass_relative_change", mass_error},
                                {"energy_relative_change", energy_error},
                                {"passed", ok}};
    }

    report["validation_errors"] = device->errorCount();
    report["passed"] = passed && device->errorCount() == 0;
    if (!report_path.empty()) {
        std::ofstream file{report_path, std::ios::binary | std::ios::trunc};
        file << report.dump(2) << '\n';
    }
    std::printf("GasValidation: %s, %u validation error(s)\n", passed ? "all checks passed" : "FAILED",
                device->errorCount());
    return passed && device->errorCount() == 0 ? 0 : 3;
}

} // namespace helios::examples

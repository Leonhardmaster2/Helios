// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Validation of the GPU gas-dynamics solver against two problems with exact
// solutions:
//
//   Sod's shock tube      a rarefaction, a contact surface and a shock; the
//                         computed density, velocity and pressure along the
//                         tube are compared with the exact solution of the
//                         Riemann problem;
//   Sedov-Taylor blast    a point explosion in a uniform gas; the radius of
//                         the spherical shock must follow
//                         R = xi0 (E t^2 / rho)^(1/5), and mass and energy
//                         must be conserved.
#pragma once

#include <cstdint>
#include <string>

namespace helios::examples {

struct GasState {
    double density = 0.0;
    double velocity = 0.0;
    double pressure = 0.0;
};

// Exact solution of the Riemann problem for an ideal gas (Toro, "Riemann
// Solvers and Numerical Methods for Fluid Dynamics", ch. 4) at the similarity
// coordinate xi = x / t.
[[nodiscard]] GasState riemannExact(GasState const& left, GasState const& right, double gamma,
                                    double xi);

// Windowless route of the lab: runs both problems on the GPU at `resolution`
// cells per edge, prints the errors, optionally writes a JSON report, and
// returns 0 when every check passes.
[[nodiscard]] int runGasValidation(std::uint32_t resolution, std::string const& report_path,
                                   bool validation_layers);

} // namespace helios::examples

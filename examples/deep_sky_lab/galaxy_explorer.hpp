// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Exploring the galaxy of the deep-sky lab: picking a star particle, a star,
// a planet or a moon, flying the camera to it, following it as the galaxy
// turns, and the clock that slows from megayears to hours per second as the
// camera closes in.  The interactive route drives this; the scene
// (galaxy_scene.hpp) does the drawing.
#pragma once

#include "galaxy_scene.hpp"

#include <string>

struct ImDrawList;

namespace helios::examples {

struct GalaxyExplorer {
    // Shown while the camera looks at the whole galaxy; closer in, time runs
    // slower in proportion to the time it takes to cross what is in view.
    double speed_at_galaxy_myr_per_second = 20.0;
    bool show_orbits = true;
    bool show_labels = true;
    // A flight to a new focus: the point looked at moves over its duration
    // and the distance changes on a log scale.
    double flight_elapsed = 0.0;
    double flight_seconds = 0.0;
    double from_distance = 0.0;
    double to_distance = 0.0;
    // The rate of the last frame (Myr per second of wall time).
    double rate_myr_per_second = 0.0;
};

// Distance (kpc) from which a focus is seen well: a star particle's
// neighbourhood, a star's planetary system, a planet with its moons.
[[nodiscard]] double viewingDistance(GalaxyScene const& scene, GalaxyFocus const& focus);

// Starts a flight from the current focus to `target`.
void flyTo(GalaxyExplorer& explorer, GalaxySettings& galaxy, GalaxyFocus const& target, double distance_kpc,
           double to_distance_kpc, double seconds = 2.5);

// The focus one level up: moon -> planet -> star -> particle -> centre.
[[nodiscard]] GalaxyFocus parentFocus(GalaxyFocus const& focus);

// Advances a flight, keeps the camera outside the focused body and sets the
// display clock's rate for this frame.
void updateExplorer(GalaxyExplorer& explorer, GalaxySettings& galaxy, GalaxyScene const& scene, double& distance_kpc,
                    double dt_seconds, bool animate);

// Panel of the focus, its system and the clock (an ImGui section).
void drawExplorerPanel(GalaxyExplorer& explorer, GalaxySettings& galaxy, GalaxyScene const& scene,
                       double& distance_kpc);

// Orbits and names over the image; `width` and `height` are the overlay's size.
void drawExplorerOverlay(GalaxyExplorer const& explorer, GalaxySettings const& galaxy, GalaxyScene const& scene,
                         ImDrawList* list, float width, float height);

// "8.2 kpc", "35 pc", "1.02 AU", "6371 km".
[[nodiscard]] std::string formatDistance(double kpc);
// "12 Myr", "3.2 yr", "4.1 days", "2.0 hours" (of simulated time).
[[nodiscard]] std::string formatDuration(double myr);

} // namespace helios::examples

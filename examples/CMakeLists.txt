# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 the Helios authors and contributors

if(HELIOS_ENABLE_DILIGENT)
  add_executable(helios_diligent_probe diligent_probe.cpp)
  target_link_libraries(helios_diligent_probe PRIVATE helios_diligent_render helios_core)
  helios_configure_target(helios_diligent_probe)
endif()

add_subdirectory(black_hole_lab)
add_subdirectory(terrain_lab)
if(HELIOS_ENABLE_DILIGENT)
  add_subdirectory(precision_lab)
  add_subdirectory(planet_lod_lab)
  add_subdirectory(deep_sky_lab)
  add_subdirectory(solar_system_lab)
endif()

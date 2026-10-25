# SPDX-License-Identifier: AGPL-3.0-only
# Module skeleton (M0). Cube-sphere/LOD/residency systems arrive in M2-M4.
add_library(helios_planet STATIC
  module.cpp
  cube_sphere.cpp
  quadtree_lod.cpp
  representation.cpp
  terrain_generator.cpp
  residency.cpp)
target_include_directories(helios_planet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(helios_planet PUBLIC helios_core helios_math helios_assets)
helios_configure_target(helios_planet)

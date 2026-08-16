# SPDX-License-Identifier: AGPL-3.0-only
add_library(helios_assets STATIC
  manifest.cpp
  report.cpp
  schema.cpp)
target_include_directories(helios_assets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(helios_assets PUBLIC helios_core helios_math)
helios_configure_target(helios_assets)

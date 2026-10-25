# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 the Helios authors and contributors

add_library(helios_blackhole STATIC
  ray_tracer.cpp
  thin_disk.cpp
  blackbody.cpp)
target_include_directories(helios_blackhole PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(helios_blackhole PUBLIC helios_math)
helios_configure_target(helios_blackhole)

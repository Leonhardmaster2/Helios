# SPDX-License-Identifier: AGPL-3.0-only
# M1: Instant, FrameSnapshot, and synthetic frame providers (M7 adds the
# real astronomical models behind the same consumer API).
add_library(helios_universe STATIC
  module.cpp
  instant.cpp
  frame_snapshot.cpp
  frame_provider.cpp
  camera_reparent.cpp)
target_include_directories(helios_universe PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(helios_universe PUBLIC helios_core helios_math helios_assets)
helios_configure_target(helios_universe)

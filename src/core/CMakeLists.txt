# SPDX-License-Identifier: AGPL-3.0-only
# helios_core: platform services, errors, logging, config, profiling, IDs,
# checked IO primitives. No internal dependencies (docs/ARCHITECTURE.md).
add_library(helios_core STATIC
  version.cpp
  error.cpp
  log.cpp
  config.cpp
  hash.cpp
  io.cpp
  debug_name.cpp
  timer.cpp
  profiler.cpp)
target_include_directories(helios_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(helios_core PUBLIC helios_json)
helios_configure_target(helios_core)
if(HELIOS_ENABLE_TRACY)
  target_link_libraries(helios_core PUBLIC Tracy::TracyClient)
  target_compile_definitions(helios_core PUBLIC TRACY_ENABLE TRACY_ON_DEMAND TRACY_NO_EXIT TRACY_NO_FRAME_IMAGE)
endif()

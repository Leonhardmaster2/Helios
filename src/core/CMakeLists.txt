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
  # NOTE: TRACY_NO_EXIT must stay OFF: it removes the worker's ShouldExit()
  # check, so the process would hang at exit waiting for a profiler.
  target_compile_definitions(helios_core PUBLIC TRACY_ENABLE TRACY_ON_DEMAND TRACY_NO_FRAME_IMAGE)
  # The profiler facade (helios/core/profiler.hpp) is only live when this
  # definition is present; without it every HELIOS_ZONE_* macro is a no-op.
  target_compile_definitions(helios_core PUBLIC HELIOS_ENABLE_TRACY)
endif()

# Engine version from project() (helios/version.hpp mirrors these defaults).
target_compile_definitions(helios_core PUBLIC
  HELIOS_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  HELIOS_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  HELIOS_VERSION_PATCH=${PROJECT_VERSION_PATCH})

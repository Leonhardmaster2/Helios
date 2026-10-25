# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 the Helios authors and contributors
#
# Helios — Milestone 0 project skeleton.
# Module dependency rules are enforced by:
#   - CMake link edges (see src/<module>/CMakeLists.txt and docs/ARCHITECTURE.md)
#   - scripts/check_module_deps.mjs (include-level audit, run in CI)

cmake_minimum_required(VERSION 3.25)

project(Helios
  VERSION 0.0.1
  DESCRIPTION "Open-source Solar-System visualization engine and reference explorer"
  LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(HELIOS_BUILD_TESTS         "Build Helios test executables" ON)
option(HELIOS_ENABLE_VALIDATION   "Enable Vulkan validation layers (dev/CI builds)" ON)
option(HELIOS_ENABLE_TRACY        "Enable Tracy profiler client" ON)
option(HELIOS_ENABLE_IMGUI        "Enable Dear ImGui debug UI" ON)
option(HELIOS_ENABLE_DILIGENT     "Build the Vulkan-only Diligent renderer and labs" ON)
option(HELIOS_ENABLE_GPU_TESTS    "Register short physical-GPU CTest checks" OFF)
option(HELIOS_WERROR              "Treat compiler warnings as errors" OFF)
option(HELIOS_ENABLE_ASAN         "Enable AddressSanitizer where supported" OFF)
option(HELIOS_ENABLE_UBSAN        "Enable UndefinedBehaviorSanitizer where supported" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Dependency management (pinned, immutable revisions — cmake/deps.lock.json)
# ---------------------------------------------------------------------------
include(cmake/HeliosCompileOptions.cmake)
include(cmake/HeliosDeps.cmake)
if(HELIOS_ENABLE_DILIGENT)
  include(cmake/Diligent.cmake)
endif()
include(cmake/SlangCompile.cmake)

# Example projects also register bounded headless checks. Testing must be
# enabled before their subdirectories are evaluated so those checks appear in
# the generated CTest graph.
if(HELIOS_BUILD_TESTS)
  enable_testing()
endif()

# ---------------------------------------------------------------------------
# Modules (dependency graph per docs/ARCHITECTURE.md)
# ---------------------------------------------------------------------------
add_subdirectory(src/core)
add_subdirectory(src/math)
add_subdirectory(src/assets)
add_subdirectory(src/universe)
add_subdirectory(src/planet)
add_subdirectory(src/blackhole)
add_subdirectory(src/vk)
add_subdirectory(src/render)
add_subdirectory(src/app)
add_subdirectory(examples)

# ---------------------------------------------------------------------------
# Shaders: offline Slang -> SPIR-V compilation + reflection manifest
# ---------------------------------------------------------------------------
helios_compile_shaders()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(HELIOS_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Convenience check targets
# ---------------------------------------------------------------------------
add_custom_target(helios-check-deps
  COMMAND node "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_module_deps.mjs"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  COMMENT "Auditing module include dependencies")

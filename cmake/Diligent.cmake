# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 the Helios authors and contributors
#
# Diligent is kept as an independently versioned recursive Git submodule.  The
# options below deliberately build one backend only: Vulkan.  Helios compiles
# its shaders offline with Slang, so Diligent's runtime shader compilers and
# unrelated backends are excluded from the reproducible v0.0.1 build.

set(HELIOS_DILIGENT_ROOT
    "${CMAKE_SOURCE_DIR}/third_party/DiligentEngine"
    CACHE PATH "DiligentEngine source root")

if(NOT EXISTS "${HELIOS_DILIGENT_ROOT}/DiligentCore/CMakeLists.txt")
  message(FATAL_ERROR
    "DiligentEngine is not initialized at ${HELIOS_DILIGENT_ROOT}. "
    "Run: git submodule update --init --recursive")
endif()

function(helios_read_dependency_commit dependency_path output_name)
  execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY "${dependency_path}"
    OUTPUT_VARIABLE dependency_commit
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(NOT dependency_commit)
    set(dependency_commit "unknown")
  endif()
  set(${output_name} "${dependency_commit}" PARENT_SCOPE)
endfunction()

helios_read_dependency_commit("${HELIOS_DILIGENT_ROOT}" HELIOS_DILIGENT_COMMIT)
helios_read_dependency_commit("${HELIOS_DILIGENT_ROOT}/DiligentCore"
                               HELIOS_DILIGENT_CORE_COMMIT)
add_compile_definitions(
  "HELIOS_DILIGENT_COMMIT=\"${HELIOS_DILIGENT_COMMIT}\""
  "HELIOS_DILIGENT_CORE_COMMIT=\"${HELIOS_DILIGENT_CORE_COMMIT}\"")
message(STATUS
  "Helios Diligent pins: engine=${HELIOS_DILIGENT_COMMIT}, core=${HELIOS_DILIGENT_CORE_COMMIT}")

# The full DiligentTools tree pulls texture loaders, asset importers, render
# state notation generators, Python packages, and their third-party codecs.
# Helios v0.0.1 needs only the small ImGui bridge, added explicitly below.
set(DILIGENT_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(DILIGENT_BUILD_FX OFF CACHE BOOL "" FORCE)
set(DILIGENT_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(DILIGENT_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(DILIGENT_BUILD_TESTS OFF CACHE BOOL "" FORCE)

set(DILIGENT_NO_DIRECT3D11 ON CACHE BOOL "" FORCE)
set(DILIGENT_NO_DIRECT3D12 ON CACHE BOOL "" FORCE)
set(DILIGENT_NO_OPENGL ON CACHE BOOL "" FORCE)
set(DILIGENT_NO_VULKAN OFF CACHE BOOL "" FORCE)
set(DILIGENT_NO_METAL ON CACHE BOOL "" FORCE)
set(DILIGENT_NO_WEBGPU ON CACHE BOOL "" FORCE)

set(DILIGENT_NO_GLSLANG ON CACHE BOOL "" FORCE)
set(DILIGENT_NO_HLSL ON CACHE BOOL "" FORCE)
set(DILIGENT_NO_ARCHIVER ON CACHE BOOL "" FORCE)
set(DILIGENT_NO_SUPER_RESOLUTION ON CACHE BOOL "" FORCE)
set(DILIGENT_NO_RENDER_STATE_PACKAGER ON CACHE BOOL "" FORCE)
set(DILIGENT_ENABLE_DRACO OFF CACHE BOOL "" FORCE)

set(DILIGENT_INSTALL_CORE OFF CACHE BOOL "" FORCE)
set(DILIGENT_INSTALL_TOOLS OFF CACHE BOOL "" FORCE)
set(DILIGENT_INSTALL_PDB OFF CACHE BOOL "" FORCE)

# Compile the Diligent ImGui bridge against Helios's single pinned ImGui copy.
if(HELIOS_ENABLE_IMGUI)
  set(DILIGENT_DEAR_IMGUI_PATH "${imgui_SOURCE_DIR}" CACHE PATH "" FORCE)
endif()

add_subdirectory(
  "${HELIOS_DILIGENT_ROOT}"
  "${CMAKE_BINARY_DIR}/_deps/diligent"
  EXCLUDE_FROM_ALL)

if(HELIOS_ENABLE_IMGUI)
  set(IMGUIZMO_QUAT_PATH
      "${HELIOS_DILIGENT_ROOT}/DiligentTools/ThirdParty/imGuIZMO.quat"
      CACHE PATH "" FORCE)
  add_subdirectory(
    "${HELIOS_DILIGENT_ROOT}/DiligentTools/Imgui"
    "${CMAKE_BINARY_DIR}/_deps/diligent/DiligentTools/Imgui"
    EXCLUDE_FROM_ALL)
  target_link_libraries(Diligent-Imgui PRIVATE SDL3::SDL3-static)
endif()

# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 the Helios authors and contributors
#
# Offline Slang -> SPIR-V compilation. slangc is located from
# HELIOS_SLANG_BIN_DIR (set by scripts/fetch_deps.mjs) or PATH.
# Pinned toolchain: shader-slang/slang v2026.14.1 (see cmake/deps.lock.json).

find_program(HELIOS_SLANGC_EXECUTABLE slangc
  HINTS "${HELIOS_SLANG_BIN_DIR}"
  DOC "slangc executable (pinned via scripts/fetch_deps.mjs)")

if(NOT HELIOS_SLANGC_EXECUTABLE)
  message(FATAL_ERROR
    "slangc not found. Run: node scripts/fetch_deps.mjs --update-lock --write-cmake\n"
    "Pinned toolchain: shader-slang/slang v2026.14.1.")
endif()

set(HELIOS_SLANG_PROFILE "cs_6_6" CACHE STRING "Slang target profile for SPIR-V output")

function(helios_compile_shaders)
  set(shader_srcs
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/depth.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/compute.slang")
  set(shader_out_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
  file(MAKE_DIRECTORY "${shader_out_dir}")

  set(outputs)
  foreach(src IN LISTS shader_srcs)
    get_filename_component(base "${src}" NAME_WE)
    set(out "${shader_out_dir}/${base}.spv")
    list(APPEND outputs "${out}")
    add_custom_command(
      OUTPUT "${out}"
      COMMAND "${HELIOS_SLANGC_EXECUTABLE}" "${src}"
              -target spirv -profile "${HELIOS_SLANG_PROFILE}"
              -O3 -g0
              -o "${out}"
      DEPENDS "${src}"
      COMMENT "slangc: ${base}.slang -> ${base}.spv"
      VERBATIM)
  endforeach()

  # Record the exact compiler version used (reproducibility evidence).
  set(version_stamp "${shader_out_dir}/slangc-version.txt")
  add_custom_command(
    OUTPUT "${version_stamp}"
    COMMAND ${CMAKE_COMMAND} -E echo "slangc ${HELIOS_SLANGC_EXECUTABLE}:" > "${version_stamp}"
    COMMAND "${HELIOS_SLANGC_EXECUTABLE}" -version >> "${version_stamp}" 2>&1
    VERBATIM)
  list(APPEND outputs "${version_stamp}")

  # Asset manifest with per-file SHA-256 (written by cmake -P script).
  set(manifest "${shader_out_dir}/shaders.manifest.json")
  list(APPEND outputs "${manifest}")
  add_custom_command(
    OUTPUT "${manifest}"
    COMMAND "${CMAKE_COMMAND}"
      -DHELIOS_MANIFEST_OUT="${manifest}"
      -DHELIOS_SHADER_DIR="${shader_out_dir}"
      -DHELIOS_SPV_FILES="${outputs}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_shader_manifest.cmake"
    DEPENDS ${outputs}
    COMMENT "Writing shader asset manifest"
    VERBATIM)

  add_custom_target(helios_shaders DEPENDS ${outputs})
  # Consumers declare add_dependencies(<tgt> helios_shaders) and locate the
  # .spv files via the HELIOS_SHADER_DIR compile definition.
endfunction()

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

function(helios_compile_shaders)
  set(shader_srcs    triangle.slang triangle.slang depth.slang depth.slang compute.slang mesh.slang mesh.slang impostor.slang impostor.slang planet_patch.slang planet_patch.slang diligent_precision.slang fullscreen.slang black_hole.slang black_hole_refine.slang hdr_glare.slang hdr_composite.slang hdr_accumulate.slang neutron_star.slang deep_sky_background.slang galaxy_stars.slang galaxy_stars.slang emission_volume.slang hydro_init.slang hydro_advance.slang hydro_speed.slang hydro_rays.slang hydro_shade.slang hydro_smooth.slang point_source_dust.slang planet_sky.slang galaxy_resolved_stars.slang galaxy_resolved_stars.slang star_system.slang solar_resolve.slang)
  set(shader_entries vsMain         fsMain         vsMain      fsMain      csMain      vsMain      fsMain      vsMain      fsMain      vsMain             fsMain             csMain                   vsMain           fsMain           fsMain                  fsMain          fsMain              fsMain               fsMain             fsMain                     vsMain             fsMain             fsMain                csMain           csMain              csMain            csMain           csMain            csMain              fsMain fsMain vsMain fsMain fsMain fsMain)
  set(shader_stages  vertex         fragment       vertex      fragment    compute     vertex      fragment    vertex      fragment    vertex             fragment           compute                  vertex           fragment         fragment                fragment        fragment            fragment             fragment           fragment                   vertex             fragment           fragment              compute          compute             compute           compute          compute           compute             fragment fragment vertex fragment fragment fragment)
  set(shader_out_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
  file(MAKE_DIRECTORY "${shader_out_dir}")

  # Files pulled in with #include are not compiled on their own, but every
  # module that includes one must be rebuilt when it changes.
  file(GLOB shader_includes CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/lib/*.slang")

  set(outputs)
  set(shader_spv_outputs)
  foreach(src_name entry stage IN ZIP_LISTS shader_srcs shader_entries shader_stages)
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${src_name}")
    set(out "${shader_out_dir}/${src_name}.${stage}.spv")
    list(APPEND outputs "${out}")
    list(APPEND shader_spv_outputs "${out}")
    add_custom_command(
      OUTPUT "${out}"
      COMMAND "${HELIOS_SLANGC_EXECUTABLE}" "${src}"
              -target spirv -entry "${entry}" -stage "${stage}"
              -O3 -g0
              -o "${out}"
      DEPENDS "${src}" ${shader_includes}
      COMMENT "slangc: ${src_name} (${stage} ${entry})"
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

  # Asset manifest with per-file SHA-256. A configure-time script with
  # baked-in paths is used (no -D args: the VS generator mangles them).
  set(manifest "${shader_out_dir}/shaders.manifest.json")
  list(APPEND outputs "${manifest}")
  set(manifest_script "${shader_out_dir}/gen_shader_manifest_impl.cmake")
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_shader_manifest_impl.in"
    "${manifest_script}" @ONLY)
  set(manifest_deps ${shader_spv_outputs} "${version_stamp}")
  add_custom_command(
    OUTPUT "${manifest}"
    COMMAND "${CMAKE_COMMAND}" -P "${manifest_script}"
    DEPENDS ${manifest_deps} "${manifest_script}"
    COMMENT "Writing shader asset manifest"
    VERBATIM)

  add_custom_target(helios_shaders DEPENDS ${outputs})

  # --- embedded shader data (self-contained binaries) -----------------------
  set(embed_hpp "${shader_out_dir}/shaders_embedded.hpp")
  set(embed_cpp "${shader_out_dir}/shaders_embedded.cpp")
  set(embed_script "${shader_out_dir}/gen_shader_embed_impl.cmake")
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_shader_embed_impl.in"
    "${embed_script}" @ONLY)
  add_custom_command(
    OUTPUT "${embed_hpp}" "${embed_cpp}"
    COMMAND "${CMAKE_COMMAND}" -P "${embed_script}"
    DEPENDS ${manifest_deps} "${manifest}" "${embed_script}"
    COMMENT "Embedding compiled shaders"
    VERBATIM)

  add_library(helios_shader_data STATIC "${embed_cpp}")
  target_include_directories(helios_shader_data PUBLIC "${shader_out_dir}")
  set_target_properties(helios_shader_data PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_dependencies(helios_shader_data helios_shaders)
  helios_configure_target(helios_shader_data)
endfunction()

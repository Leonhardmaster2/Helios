# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 the Helios authors and contributors
#
# Shared compile configuration: warnings, sanitizers, and global compile
# definitions (git hash, shader dir, build type).

# --- build provenance definitions (must be set before add_subdirectory) ----
execute_process(
  COMMAND git rev-parse HEAD
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  OUTPUT_VARIABLE HELIOS_GIT_HASH
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT HELIOS_GIT_HASH)
  set(HELIOS_GIT_HASH "unknown")
endif()
string(REPLACE "\\" "/" HELIOS_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
add_compile_definitions(
  "HELIOS_GIT_HASH=\"${HELIOS_GIT_HASH}\""
  "HELIOS_SHADER_DIR=\"${HELIOS_SHADER_DIR}\""
  "HELIOS_BUILD_TYPE=\"$<CONFIG>\""
  "HELIOS_CMAKE_VERSION=\"${CMAKE_VERSION}\"")

function(helios_configure_target tgt)
  target_compile_features(${tgt} PUBLIC cxx_std_20)

  if(MSVC)
    target_compile_options(${tgt} PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8)
    target_compile_definitions(${tgt} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(HELIOS_WERROR)
      target_compile_options(${tgt} PRIVATE /WX)
    endif()
    if(HELIOS_ENABLE_ASAN)
      target_compile_options(${tgt} PRIVATE /fsanitize=address)
      target_link_options(${tgt} PRIVATE /fsanitize=address)
    endif()
  else()
    target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(HELIOS_WERROR)
      target_compile_options(${tgt} PRIVATE -Werror)
    endif()
    if(HELIOS_ENABLE_ASAN)
      target_compile_options(${tgt} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
      target_link_options(${tgt} PRIVATE -fsanitize=address)
    endif()
    if(HELIOS_ENABLE_UBSAN)
      target_compile_options(${tgt} PRIVATE -fsanitize=undefined -fno-sanitize-recover=all)
      target_link_options(${tgt} PRIVATE -fsanitize=undefined)
    endif()
  endif()

  if(HELIOS_ENABLE_TRACY AND NOT MSVC)
    target_link_libraries(${tgt} PRIVATE Tracy::TracyClient)
  endif()
endfunction()

# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 the Helios authors and contributors
#
# Pinned dependency wiring. The canonical fetch path is
#   node scripts/fetch_deps.mjs --update-lock --write-cmake
# which verifies SHA-256 against cmake/deps.lock.json, extracts sources into
# .deps-cache/ and writes cmake/deps-cache.cmake (gitignored). When that file
# exists, FetchContent uses the cached sources directly (fully offline).
# Without it, FetchContent falls back to the pinned URL+hash declared below.

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/deps-cache.cmake")
  include("${CMAKE_CURRENT_LIST_DIR}/deps-cache.cmake")
endif()

include(FetchContent)

# --- SDL3 (zlib license) ---------------------------------------------------
FetchContent_Declare(SDL
  URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.14.tar.gz
  URL_HASH SHA256=9d57b178fb297e121ef2605275937b7afaa7cd24d99ce1f95953e69e7a2535d6
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)

# --- Vulkan-Headers (Apache-2.0) -------------------------------------------
FetchContent_Declare(VulkanHeaders
  URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.357.0.tar.gz
  URL_HASH SHA256=e87dce08116151f6b6d7de6b6faf41498e87e6cf848ff16fa3bd5402190ad4a3
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# --- Tracy client (BSD-3-Clause) -------------------------------------------
FetchContent_Declare(Tracy
  URL https://github.com/wolfpld/tracy/archive/refs/tags/v0.14.0.tar.gz
  URL_HASH SHA256=a932cf2a90adbf63f87b449fa4374a52f18a36c4a3858d4d69d3e75d62fa5f6a
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(TRACY_STATIC ON CACHE BOOL "" FORCE)
set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
set(TRACY_NO_FRAME_IMAGE ON CACHE BOOL "" FORCE)
set(TRACY_NO_EXIT ON CACHE BOOL "" FORCE)

# --- Dear ImGui (MIT) ------------------------------------------------------
FetchContent_Declare(ImGui
  URL https://github.com/ocornut/imgui/archive/46d39d56febc2a00bdd2270dc88c8a13f2a0441a.tar.gz
  URL_HASH SHA256=2dcf01525add00deea85de961b76c9255060bd0a338b7bb02af2655425b827d3
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# --- SPIRV-Reflect (Apache-2.0) --------------------------------------------
set(SPIRV_REFLECT_STATIC_LIB ON CACHE BOOL "" FORCE)
set(SPIRV_REFLECT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SpirvReflect
  URL https://github.com/KhronosGroup/SPIRV-Reflect/archive/refs/tags/vulkan-sdk-1.4.357.0.tar.gz
  URL_HASH SHA256=c865bd55459c5b8020a6ac962e462fc33eb4bf2dae8bf7c474357c58ce22a95d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_MakeAvailable(SDL VulkanHeaders Tracy ImGui SpirvReflect)

include("${CMAKE_CURRENT_LIST_DIR}/ImGui.cmake")

# --- Vendored single-header libraries (all MIT, pinned in deps.lock.json) --
add_library(helios_volk STATIC "${CMAKE_CURRENT_LIST_DIR}/../third_party/volk/volk.c")
target_include_directories(helios_volk PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../third_party/volk")
target_compile_definitions(helios_volk PUBLIC VK_NO_PROTOTYPES)
set_target_properties(helios_volk PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(helios_vma INTERFACE)
target_include_directories(helios_vma INTERFACE "${CMAKE_CURRENT_LIST_DIR}/../third_party/VMA")

add_library(helios_json INTERFACE)
target_include_directories(helios_json INTERFACE "${CMAKE_CURRENT_LIST_DIR}/../third_party/nlohmann")

add_library(helios_doctest INTERFACE)
target_include_directories(helios_doctest INTERFACE "${CMAKE_CURRENT_LIST_DIR}/../third_party")

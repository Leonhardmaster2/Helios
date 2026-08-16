# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 the Helios authors and contributors
#
# Generates shaders.manifest.json — schema-versioned asset manifest with
# per-file SHA-256, matching the Helios asset-manifest contract
# (docs/DATA_AND_PROVENANCE.md, schemas/asset-manifest.schema.json).

file(SHA256 "${HELIOS_SLANGC_EXECUTABLE}" slangc_hash)

set(entries "[]")
foreach(spv IN LISTS HELIOS_SPV_FILES)
  get_filename_component(fname "${spv}" NAME)
  file(SHA256 "${spv}" fhash)
  file(SIZE "${spv}" fsize)
  string(JSON entry OBJECT
    "path" "${fname}"
    "sha256" "${fhash}"
    "size_bytes" "${fsize}"
    "class" "model-derived"
    "role" "compiled-shader")
  string(JSON entries SET "${entries}" -1 "${entry}")
endforeach()

string(JSON manifest OBJECT
  "schema_version" 1
  "format" "helios-asset-manifest"
  "generator" "cmake/gen_shader_manifest.cmake + slangc (pinned v2026.14.1)"
  "slangc_sha256" "${slangc_hash}"
  "entries" "${entries}")

file(WRITE "${HELIOS_MANIFEST_OUT}" "${manifest}\n")
message(STATUS "shader manifest: ${HELIOS_MANIFEST_OUT}")

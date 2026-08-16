# Reproduce the M0 evidence

Reference profile: Windows 11, MSVC 2022 (vcvars64), CMake ≥ 3.25, Node.js ≥ 20.

```powershell
# 1. Fetch pinned dependencies (SHA-256 verified)
node scripts/fetch_deps.mjs --update-lock --write-cmake

# 2. Configure + build (Visual Studio generator on Windows; ninja presets on Linux/macOS CI)
cmake --preset windows-dev
cmake --build --preset windows-dev

# 3. Unit + shader ABI tests
ctest --preset windows-dev --output-on-failure

# 4. Headless smoke with validation layers (writes capabilities/performance/evidence JSON)
build/windows-dev/src/app/RelWithDebInfo/helios.exe --headless-smoke --iterations 3 --report-dir evidence/m0/local

# 5. Windowed smoke (SDL3 window, ImGui overlay; closes itself after N frames)
build/windows-dev/src/app/RelWithDebInfo/helios.exe --frames 400 --report-dir evidence/m0/perf

# 5b. Swapchain-recreation exercise (window resized twice mid-run, validation on)
build/windows-dev/src/app/RelWithDebInfo/helios.exe --frames 90 --resize-test --report-dir evidence/m0/resize

# 5c. MSVC ASan unit tests (asan runtime DLL must be on PATH, e.g. from vcvars64.bat)
cmake --preset windows-asan; cmake --build --preset windows-asan
ctest --test-dir build/windows-asan -C Debug --output-on-failure

# 6. Formatting check (clang-format 15+)
#    clang-format -style=file --dry-run --Werror $(Get-ChildItem src,tests -Recurse -Include *.cpp,*.hpp,*.h | % FullName)

# 7. Module dependency audit
node scripts/check_module_deps.mjs
```

## Blocked items and procedures

- **CI execution:** push to GitHub and open a PR; the workflow
  (`.github/workflows/ci.yml`) builds Ubuntu/GCC, Ubuntu/Clang, Windows/MSVC
  and macOS/Clang CPU suites plus the ASan/UBSan job. The `gpu-smoke` job
  requires a self-hosted runner labelled `gpu`.
- **macOS/MoltenVK tier:** on a macOS machine with Xcode + the Vulkan SDK:
  `cmake --preset ci && cmake --build --preset ci`, run the unit/shader
  tests, then `helios --headless-smoke` with validation; record the
  MoltenVK + Metal versions in the report and add the device row to the
  evidence matrix.
- **AMD/Intel device matrix:** run step 4 on each device and archive the
  report directory.
- **Gaia Archive experiment (D-019):** execute the ADQL recipe (to be pinned
  with M9), archive the query result, hash it, and validate the page counts
  against the D-019 shape.
- **Earth hero region (D-020):** download the exact SRTM/NASADEM product
  tiles, record product page, license, DOI, datum, and SHA-256, and validate
  against the acceptance criteria in DATA_AND_PROVENANCE.md.

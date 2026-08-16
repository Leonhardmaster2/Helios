# SPDX-License-Identifier: AGPL-3.0-only
# clang-tidy over compile_commands.json (run after configure).
# Usage: powershell -File scripts/run_tidy.ps1 [-BuildDir build/dev]
param([string]$BuildDir = "build/dev")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$cc = Join-Path $root "$BuildDir/compile_commands.json"
if (-not (Test-Path $cc)) {
  Write-Host "compile_commands.json not found; configure first (cmake --preset dev -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)"
  exit 2
}
$tidy = Get-Command clang-tidy -ErrorAction SilentlyContinue
if (-not $tidy) {
  Write-Host "clang-tidy not found in PATH" -ForegroundColor Yellow
  exit 2
}
$files = Get-ChildItem -Path (Join-Path $root "src") -Recurse -Include *.cpp |
  Where-Object { $_.FullName -notmatch "third_party" }
$fail = 0
foreach ($f in $files) {
  & $tidy.Source -p (Join-Path $root $BuildDir) $f.FullName
  if ($LASTEXITCODE -ne 0) { $fail = 1 }
}
if ($fail -ne 0) { Write-Host "clang-tidy found issues" -ForegroundColor Red; exit 1 }
Write-Host "OK: clang-tidy clean"

# SPDX-License-Identifier: AGPL-3.0-only
# Formatting check (clang-format). Exits non-zero when files differ.
# Usage: powershell -File scripts/check_format.ps1
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$clangFormat = Get-Command clang-format -ErrorAction SilentlyContinue
if (-not $clangFormat) {
  Write-Host "clang-format not found in PATH; install LLVM (>= 16)"
  exit 2
}
$files = Get-ChildItem -Path (Join-Path $root "src"), (Join-Path $root "tests") -Recurse -Include *.hpp,*.cpp,*.h |
  Where-Object { $_.FullName -notmatch "third_party" }
$dirty = @()
foreach ($f in $files) {
  & $clangFormat.Source -style=file --dry-run --Werror $f.FullName 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) { $dirty += $f.FullName }
}
if ($dirty.Count -gt 0) {
  Write-Host "Formatting violations:"
  $dirty | ForEach-Object { Write-Host "  $_" }
  Write-Host "Run: clang-format -i <files>"
  exit 1
}
Write-Host "OK: $($files.Count) files formatted"

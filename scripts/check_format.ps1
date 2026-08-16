# SPDX-License-Identifier: AGPL-3.0-only
# Formatting check (clang-format). Exits non-zero when files differ.
# Usage: powershell -File scripts/check_format.ps1
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$clangFormat = Get-Command clang-format -ErrorAction SilentlyContinue
if (-not $clangFormat) {
  Write-Host "clang-format not found in PATH; install LLVM or use scripts/fetch_tools.mjs" -ForegroundColor Yellow
  exit 2
}
$files = Get-ChildItem -Path (Join-Path $root "src") -Recurse -Include *.hpp,*.cpp,*.h |
  Where-Object { $_.FullName -notmatch "third_party" }
$dirty = @()
foreach ($f in $files) {
  $tmp = [System.IO.Path]::GetTempFileName()
  & $clangFormat.Source -style=file $f.FullName | Out-File -Encoding utf8 $tmp
  if (-not (Compare-Object (Get-Content $f.FullName) (Get-Content $tmp))) {
    Remove-Item $tmp -Force
    continue
  }
  $dirty += $f.FullName
  Remove-Item $tmp -Force
}
if ($dirty.Count -gt 0) {
  Write-Host "Formatting violations:"
  $dirty | ForEach-Object { Write-Host "  $_" }
  Write-Host "Run: clang-format -i <files>"
  exit 1
}
Write-Host "OK: $($files.Count) files formatted"

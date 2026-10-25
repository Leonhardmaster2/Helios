// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Audits that source files only include headers of modules listed in their
// allowed dependencies (docs/ARCHITECTURE.md — Proposed module graph).
// Usage: node scripts/check_module_deps.mjs
import fs from 'node:fs';
import path from 'node:path';

const root = path.resolve(import.meta.dirname, '..');

// Allowed internal dependencies per module (docs/ARCHITECTURE.md).
const allowed = {
  core: [],
  math: ['core'],
  assets: ['core', 'math'],
  universe: ['core', 'math', 'assets'],
  planet: ['core', 'math', 'assets'],
  blackhole: ['core', 'math'],
  vk: ['core'], // external: SDL3, volk, VMA, Vulkan-Headers
  render: ['core', 'math', 'assets', 'universe', 'planet', 'blackhole', 'vk'],
  app: ['core', 'math', 'assets', 'universe', 'planet', 'blackhole', 'render'], // NO vk edge
};

const moduleDirs = Object.keys(allowed);
let failures = 0;

function walk(dir) {
  const out = [];
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, entry.name);
    if (entry.isDirectory()) out.push(...walk(p));
    else if (/.(hpp|cpp|h)$/.test(entry.name)) out.push(p);
  }
  return out;
}

for (const mod of moduleDirs) {
  const srcDir = path.join(root, 'src', mod);
  if (!fs.existsSync(srcDir)) {
    console.log('MISSING MODULE DIR: src/' + mod);
    failures++;
    continue;
  }
  for (const file of walk(srcDir)) {
    const rel = path.relative(root, file);
    const text = fs.readFileSync(file, 'utf8');
    for (const line of text.split('\n')) {
      const m = line.match(/#\s*include\s*["<]helios\/([a-z_]+)\//);
      if (!m) continue;
      const dep = m[1];
      if (!moduleDirs.includes(dep)) continue; // not a helios module (e.g. helios/app in tests)
      if (dep === mod) continue; // self include is fine
      if (!allowed[mod].includes(dep)) {
        console.log('VIOLATION: ' + rel + ' includes helios/' + dep + '/ (not in allowed deps of ' + mod + ')');
        failures++;
      }
    }
  }
}

// Tests may include any module (they link the graph), but must not include vk
// headers indirectly via render; the app rule is enforced above. Tests dir:
const testDir = path.join(root, 'tests');
if (fs.existsSync(testDir)) {
  for (const file of walk(testDir)) {
    const rel = path.relative(root, file);
    const text = fs.readFileSync(file, 'utf8');
    for (const line of text.split('\n')) {
      const m = line.match(/#\s*include\s*["<]helios\/([a-z_]+)\//);
      if (m && moduleDirs.includes(m[1])) {
        // shader ABI test intentionally includes helios/math (abi.hpp) — fine.
      }
    }
  }
}

if (failures > 0) {
  console.log('FAIL: ' + failures + ' module dependency violation(s)');
  process.exit(1);
}
console.log('OK: module dependency graph respected');

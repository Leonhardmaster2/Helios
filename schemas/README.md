# Helios schemas

Versioned JSON Schema (draft-07) documents for Helios reports and manifests.

| File | Format id | Version | Purpose |
|---|---|---|---|
| `asset-manifest.schema.json` | `helios-asset-manifest` | 1 | Asset manifests (shaders, terrain, catalogs) |
| `capability-report.schema.json` | `helios-capability-report` | 1 | Vulkan device capability report |
| `performance-report.schema.json` | `helios-performance-report` | 1 | Frame-time/performance evidence |
| `evidence-report.schema.json` | `helios-evidence-report` | 1 | Milestone evidence bundle summary |

The C++ writers in `src/assets` are the authoritative producers; these
documents describe the same contract. A schema version bump is required for
any incompatible change; readers reject newer versions.

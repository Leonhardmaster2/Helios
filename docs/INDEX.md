# Documentation index

This is the front door for Helios's design and implementation documentation. Source and tests now
exist, but each claim still keeps an explicit proposed/implemented/validated status; source code
alone does not promote a result to validated.

Last updated: 25 October 2026.

## Status vocabulary

Every important claim should be understood through one of these states:

| State | Meaning |
|---|---|
| **Proposed** | A candidate product or technical choice that may change during a spike |
| **Accepted** | The current direction; changing it requires new evidence and a decision-log update |
| **Implemented** | Present in source code and exercised by at least one automated or manual path |
| **Validated** | Meets a recorded acceptance threshold on named hardware/data, with evidence |

Unless a document explicitly says otherwise, product scope and logged decisions are **accepted**
and the full v1 architecture remains **proposed**. The narrower v0.0.1 implementation state and
its remaining visual acceptance gate are recorded in [V0_0_1_MVP.md](V0_0_1_MVP.md) and the
root [README](../README.md); no performance claim becomes validated without captured evidence.

## Source-of-truth hierarchy

When two documents disagree, use this ownership order rather than guessing:

| Question | Authoritative document |
|---|---|
| What product are we making? What is in or out of v1? | [PRODUCT.md](PRODUCT.md) |
| What is the current status and what should a newcomer read? | [README.md](../README.md) and this index |
| In what order will work ship? | [ROADMAP.md](../ROADMAP.md) |
| How is the software intended to work? | [ARCHITECTURE.md](ARCHITECTURE.md) |
| What does the first integrated MVP promise? | [V0_0_1_MVP.md](V0_0_1_MVP.md) |
| What is new, and what is explicitly not claimed, in v0.0.1? | [RELEASE_NOTES_v0.0.1.md](RELEASE_NOTES_v0.0.1.md) |
| Why was a contested choice made? | [DECISIONS.md](DECISIONS.md) |
| What exact claim must be proved? | [VALIDATION.md](VALIDATION.md) |
| What data is used and what may we say about it? | [DATA_AND_PROVENANCE.md](DATA_AND_PROVENANCE.md) |
| Which platform or GPU is supported? | [SUPPORT.md](SUPPORT.md) |
| Is the total plan credible? | [FEASIBILITY.md](FEASIBILITY.md) |
| What do the terms mean? | [GLOSSARY.md](GLOSSARY.md) |
| What may contributors submit? | [CONTRIBUTING.md](../CONTRIBUTING.md) |
| What licenses apply? | [LICENSING.md](../LICENSING.md), [LICENSE](../LICENSE), and dataset manifests |

A decision record can constrain architecture, but it does not silently add product scope. A roadmap item can schedule a requirement, but it does not redefine that requirement. Validation evidence can reveal that an accepted design is wrong; that should trigger a new decision rather than hiding the result.

## Reading paths

### I just found the project

1. [README.md](../README.md)
2. [PRODUCT.md](PRODUCT.md)
3. [ROADMAP.md](../ROADMAP.md)
4. [FEASIBILITY.md](FEASIBILITY.md)

### I want to implement it

1. [M0_HANDOVER.md](M0_HANDOVER.md) — full M0 engineering record: what was built, why, every bug + root cause, verification matrix, pitfalls, and the M1 start guide
2. [V0_0_1_MVP.md](V0_0_1_MVP.md)
3. [ARCHITECTURE.md](ARCHITECTURE.md)
4. [DECISIONS.md](DECISIONS.md)
5. [VALIDATION.md](VALIDATION.md)
6. [SUPPORT.md](SUPPORT.md)
7. [DATA_AND_PROVENANCE.md](DATA_AND_PROVENANCE.md)
8. [CONTRIBUTING.md](../CONTRIBUTING.md)

### I want to evaluate the scientific or data claims

1. [DATA_AND_PROVENANCE.md](DATA_AND_PROVENANCE.md)
2. [VALIDATION.md](VALIDATION.md)
3. The astronomy, coordinate, terrain, and HDR sections of [ARCHITECTURE.md](ARCHITECTURE.md)
4. [FEASIBILITY.md](FEASIBILITY.md)

### I want to use or commercially license Helios

There is no usable build or operational commercial-license process yet. Read [LICENSING.md](../LICENSING.md) for the intended model and its unresolved prerequisites. Do not treat the draft [CLA.md](../CLA.md) as a signable agreement.

## Current repository map

```text
README.md                    concise project front door
ROADMAP.md                   ordered delivery plan; Milestone 10 is v1
CONTRIBUTING.md              current contribution policy
LICENSE                      root AGPL-3.0 text; see LICENSING for the current file matrix
LICENSING.md                 draft plain-language licensing policy
CLA.md                       unreviewed contributor-agreement draft
THIRD_PARTY_NOTICES.md       software dependency versions, pins, licences, and usage
assets/
  banner.png                 project banner
docs/
  INDEX.md                   this navigation and authority map
  PRODUCT.md                 product definition and v1 requirements
  FEASIBILITY.md             claim, effort, and risk assessment
  ARCHITECTURE.md            intended technical structure
  DECISIONS.md               accepted decisions and revisit triggers
  VALIDATION.md              tests, thresholds, and evidence rules
  DATA_AND_PROVENANCE.md     data classification and manifest contract
  SUPPORT.md                 platform, GPU, and performance policy
  GLOSSARY.md                domain terminology
  V0_0_1_MVP.md              integrated planet/LOD/black-hole MVP contract
```

Implemented source directories:

```text
src/          application and library sources
shaders/      Slang sources and shader ABI tests
tools/        offline data/catalog bakers
tests/        unit, property, integration, and numeric tests
cmake/        build helpers and dependency pins
examples/     independent planet, terrain, precision, and black-hole labs
third_party/  vendored code plus the recursive DiligentEngine fork submodule
data/         small manifests only; large datasets stay external
evidence/     generated milestone reports or links to release artifacts
```

## Keeping the documents honest

- Use future tense for planned behavior and present tense only for repository facts.
- Do not upgrade a claim from accepted to implemented or validated without linking evidence.
- Give every numeric promise a range, unit, scenario, threshold, and reference.
- Distinguish source-data resolution, generated sample spacing, numerical precision, visual stability, and model accuracy; they are different quantities.
- Record exact dataset releases and licenses instead of naming an agency or family of products.
- Keep long-term ideas below the Milestone 10 gate unless a decision deliberately changes v1.

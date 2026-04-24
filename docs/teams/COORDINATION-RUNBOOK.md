# Coordination Runbook

**Purpose:** Coordinate cross-team ownership for platform kernel, global control-plane, package distribution, and docs delivery changes.

**Last updated:** 2026-04-24

## Mission

Keep `styio-platform` aligned with upstream compiler and package-manager repos
while it becomes the global cloud-computing, regional deployment, and package
distribution foundation for `styio-spio`.

## Module Map

- `src/SpioPlan/` and `src/SpioCloud/` provide the migrated platform kernel.
- `docs/governance/Platform-Workspace-Compile-Model.md` owns the Styio default target, C++/LLVM workspace, and mixed compile rule.
- `contracts/` publishes compile-plan, hosted control-plane, registry control-plane, and registry v2 packages.
- `docs/registry/` owns package distribution and mirror synchronization rules.
- `docs/operations/` owns regional node deployment and recovery rules.
- `scripts/` owns server tools, generated API artifacts, docs gates, and stress harnesses.
- `docs/` owns service governance and migration history.

## Ownership Table

| Surface | Owner |
|---------|-------|
| `src/`, `tests/`, workspace compile model | Platform Kernel |
| `contracts/`, server scripts, regional nodes, mirrors | Control Plane |
| `docs/`, docs scripts | Docs Delivery |

## Review Matrix

Platform kernel changes need native tests and must preserve the Styio-default,
C++/LLVM-capable workspace model. Control-plane changes need contract or script
gates. Regional node and mirror changes need explicit authority, freshness, and
recovery rules. Docs changes need docs audit and runbook ownership updates.

## Escalation Rules

If a change requires compiler behavior, update the `styio` handoff. If a change
requires package-manager CLI behavior, update the `styio-spio` handoff before
platform closure.
If a change affects offline package behavior, keep `styio-spio` local-first
requirements authoritative and expose only the platform distribution contract
here.
If a change affects native C++ fallback or mixed compile execution, coordinate
with `styio` for compiler semantics and keep platform workspace envelopes
explicit.

## Checkpoint Policy

Local checkpoint closure requires CMake tests, Python tests, docs audit, and
repo hygiene for touched surfaces.

## Release / Cutover Gates

Cutover from `styio-spio` is allowed only after platform CI owns equivalent
server-side checks and `styio-spio` has client-only compatibility coverage.

## Handoff / Recovery

When a platform migration fails, keep `styio-spio` compatibility shims in place
and record the missing platform gate in `docs/planning/`.

# Platform Kernel Runbook

**Purpose:** Own the migrated compile-plan, mixed Styio/C++ compile model, and cloud job request kernel.

**Last updated:** 2026-04-24

## Mission

Maintain the platform-side kernel that validates compile-plan generation,
cloud execution policy, native C++/LLVM fallback, mixed Styio/C++ compile
handoff, and build job request payloads.

## Owned Surface

- `src/SpioCore/`, `src/SpioManifest/`, `src/SpioResolve/`, and supporting imported client dependencies.
- `src/SpioPlan/` compile-plan generation.
- `src/SpioCloud/` cloud execution and job request contracts.
- `docs/governance/Platform-Workspace-Compile-Model.md`
- `tests/native/` platform kernel tests.

## Daily Workflow

Build with CMake, run native tests, and keep imported compatibility surfaces
small enough to replace with shared SDK contracts later. Treat native C++/LLVM
fallback as part of the workspace contract, not as an unrelated service path.

## Change Classes

Kernel changes include compile-plan schema behavior, cloud execution policy,
Styio/C++ target selection, native C++ invocation, fallback payload shape, job
request payload shape, or imported package-manager dependency changes.

## Required Gates

Run `cmake --build build-codex` and `ctest --test-dir build-codex --output-on-failure`.

## Cross-Team Dependencies

Coordinate with Control Plane for contract package changes, with upstream
`styio` for compiler semantics, and with upstream `styio-spio` for resolver or
manifest semantics.

## Handoff / Recovery

If a platform kernel change breaks compatibility, preserve the existing
`styio-spio` client behavior and document the cutover blocker in planning docs.

# Platform Kernel Runbook

**Purpose:** Own the migrated compile-plan, mixed Styio/C++ compile model, cloud job request kernel, and C++ service-kernel integration boundary.

**Last updated:** 2026-04-24

## Mission

Maintain the platform-side kernel that validates compile-plan generation,
cloud execution policy, native C++/LLVM fallback, mixed Styio/C++ compile
handoff, and build job request payloads.

## Owned Surface

- `src/SpioCore/`, `src/SpioManifest/`, `src/SpioResolve/`, and supporting imported client dependencies.
- `src/SpioPlan/` compile-plan generation.
- `src/SpioCloud/` cloud execution and job request contracts.
- `src/PlatformService/` native service-kernel implementation once promoted.
- `contracts/platform-control-plane/` payload shape in coordination with Control Plane.
- `docs/governance/Platform-Workspace-Compile-Model.md`
- `tests/native/` platform kernel tests.

## Daily Workflow

Build with CMake, run native tests, and keep imported compatibility surfaces
small enough to replace with shared SDK contracts later. Treat native C++/LLVM
fallback as part of the workspace contract, not as an unrelated service path.
For the V1 service kernel, keep C++ service code aligned with the native JSON
platform-control-plane contract rather than relying on generated API artifacts.

## Change Classes

Kernel changes include compile-plan schema behavior, cloud execution policy,
Styio/C++ target selection, native C++ invocation, fallback payload shape, job
request payload shape, platform service payload shape, or imported
package-manager dependency changes.

## Required Gates

Run `cmake --build build-codex` and `ctest --test-dir build-codex --output-on-failure`.

## Cross-Team Dependencies

Coordinate with Control Plane for native JSON contract package changes, with upstream
`styio` for compiler semantics, and with upstream `styio-spio` for resolver or
manifest semantics.

## Handoff / Recovery

If a platform kernel change breaks compatibility, preserve the existing
`styio-spio` client behavior and document the cutover blocker in planning docs.

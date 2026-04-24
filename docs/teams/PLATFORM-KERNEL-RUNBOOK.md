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
- `src/PlatformService/` registry control-plane route family for `/api/spio-registry-control/v1/status|publish|verify`, including redaction, mTLS role checks, local filesystem adapter behavior, and mirror freshness state.
- `src/spio_registry_v2/` migrated registry v2 service helpers used by platform-local control-plane scripts and tests.
- `contracts/platform-control-plane/` payload shape in coordination with Control Plane.
- `docs/governance/Platform-Workspace-Compile-Model.md`
- `tests/native/` platform kernel tests.

## Daily Workflow

Build with CMake, run native tests, and keep imported compatibility surfaces
small enough to replace with shared SDK contracts later. Treat native C++/LLVM
fallback as part of the workspace contract, not as an unrelated service path.
For the V1 service kernel, keep C++ service code aligned with the native JSON
platform-control-plane contract rather than relying on generated API artifacts.
For registry service work, keep platform as the hosted publish/verify/mirror
owner while preserving `styio-spio` offline client behavior and the shared
registry-control-plane v1 route shape.

## Change Classes

Kernel changes include compile-plan schema behavior, cloud execution policy,
Styio/C++ target selection, native C++ invocation, fallback payload shape, job
request payload shape, platform service payload shape, or imported
package-manager dependency changes. Registry kernel changes also include mTLS
role policy, publish/verify status code semantics, mirror freshness transitions,
and migrated registry v2 helper behavior.

## Required Gates

Run `cmake --build build-codex` and `ctest --test-dir build-codex --output-on-failure`.
For registry kernel changes, also run `python3 tests/unit/test_registry_v2.py`
and the native registry route tests in `styio_platform_native_tests`.

## Cross-Team Dependencies

Coordinate with Control Plane for native JSON contract package changes, with upstream
`styio` for compiler semantics, and with upstream `styio-spio` for resolver or
manifest semantics. Coordinate registry route and mirror behavior with
`styio-spio` so client fetch/offline semantics remain compatible.

## Handoff / Recovery

If a platform kernel change breaks compatibility, preserve the existing
`styio-spio` client behavior and document the cutover blocker in planning docs.

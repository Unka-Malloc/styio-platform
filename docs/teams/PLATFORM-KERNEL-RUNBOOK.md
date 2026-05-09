# Platform Kernel Runbook

**Purpose:** Own the migrated compile-plan, mixed Styio/C++ compile model, cloud job request kernel, and C++ service-kernel integration boundary.

**Last updated:** 2026-05-09

## Mission

Maintain the platform-side kernel that validates compile-plan generation,
cloud execution policy, native C++/LLVM fallback, mixed Styio/C++ compile
handoff, and build job request payloads.

## Owned Surface

- `src/PlatformCore/` core package, resolver, registry-client, and toolchain dependencies.
- `src/PlatformCore/System/` config loading, filesystem/process helpers, and operating-system adapters.
- `src/PlatformStorage/PlatformCache/` cache and local state layout shared by resolver, registry-client, source-build toolchain, and protocol payloads.
- `src/PlatformStorage/PlatformPersistence/` persisted state records, memory-backed development state, Postgres migrations/state access, and object storage access for platform artifacts and registry objects.
- `src/SpioPlatformProtocols/` protocol payloads and serializers, including compile-plan generation, project-graph envelopes, cloud execution policy, and job request contracts.
- `src/PlatformService/` native service-kernel implementation and local
  Boost.Beast/Asio HTTP adapter with POSIX fallback.
- `src/PlatformService/` registry control-plane route family for
  `/api/spio-registry-control/v1/status|descriptor|publish|verify`, including
  redaction, mTLS role checks, C++ signed registry v2 metadata, S3 object-store
  publication, local staging behavior, and mirror freshness state.
- `src/PlatformSecurity/PlatformCA/` certificate authority, trust-anchor lifecycle, and managed mTLS certificate issuance.
- `src/PlatformSecurity/PlatformClientAuth/` mTLS identity parsing, platform role recognition, internal role checks, and operation-level authorization policy.
- `src/PlatformSecurity/SecurityHardening/` registry security policy hooks and hardening extension points.
- `src/PlatformCloud/PackageRegistry/` package registry mirror helpers and registry v2 tooling.
- `src/PlatformCloud/DeveloperWorkspace/` cloud job queue semantics, worker runtime factories, compile-container/workspace factories, and workspace compile stress harnesses.
- `manifests/` business capability boundaries for platform foundation, package registry, and developer workspace extraction planning.
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
registry-control-plane v1 route shape. When deployment scripts need a new
registry helper, keep it covered by Python unit tests and avoid making the
client-side package manager depend on platform availability.
For PostgreSQL-backed platform state, keep the no-driver fallback and libpq
build path both compiling; CI installs `libpq-dev`, so RAII wrappers around
`PGconn` or `PGresult` must be explicitly move-safe when returned from helpers.
Worker job changes must preserve real-build defaults. Use `workflow.dry_run`
only for smoke paths that need to verify clone, scheduling, artifact writeback,
and control-plane completion without provisioning a full source-build toolchain.
Worker control-plane transport changes must keep HTTP compatibility for local
smoke tests and HTTPS client-certificate support for direct mTLS deployments.
Tool-release helper tests must preserve the static read-plane layout consumed
by the `styio-spio` installer and avoid coupling package-manager behavior to
the platform control plane at install time.
User-bound compile container changes must keep the memory backend, Postgres
backend, worker registration/claim path, and native JSON payload serialization
consistent. Container-aware claims must filter by tenant/user binding before
switching workspace state.
Cache layout changes must keep resolver offline behavior, registry materialization,
source-build roots, and project `.spio` state payloads aligned.
Storage changes must keep the memory-backed state fallback, Postgres-backed
state, object-store behavior, migrations, and persisted record serializers
aligned.
Operating-system integration changes must route environment access, filesystem
mutation, child-process execution, socket I/O, and sleeps through
`PlatformCore/System` adapters instead of adding direct OS calls to service or
cloud workflow code.
Compile container and worker workspace creation changes must go through the
factory classes so registration payloads, stable container checkout paths, and
artifact roots stay consistent.

## Change Classes

Kernel changes include compile-plan schema behavior, cloud execution policy,
Styio/C++ target selection, native C++ invocation, fallback payload shape, job
request payload shape, platform service payload shape, or imported
package-manager dependency changes. Registry kernel changes also include mTLS
role policy in `PlatformSecurity/PlatformClientAuth`, TLS listener behavior,
S3 object-store signing/upload behavior, publish/verify status code semantics,
mirror freshness transitions, VM initialization helper behavior, and migrated
registry v2 helper behavior.
Compile container kernel changes include `user_id` job serialization, Postgres
migrations, worker environment configuration, workspace generation updates, and
the route handlers that enforce container ownership.
Cache kernel changes include project state roots, `SPIO_HOME` root selection,
managed tool roots, source checkout/build roots, registry metadata/blob/checkouts,
and server-side registry state roots.
Storage kernel changes include `PlatformJobRecord`, `CompileContainerRecord`,
job events, memory state store behavior, object store provider behavior,
Postgres SQL migrations, and state store error semantics.
OS adapter changes include the interface in `PlatformCore/System`, the default
POSIX implementation, and call sites that need injectable system behavior.
Worker factory changes include compile-container specs, registration payload
creation, and workspace checkout/artifact path creation.

## Required Gates

Run `cmake --build build-codex` and `ctest --test-dir build-codex --output-on-failure`.
For HTTP adapter changes, include
`ctest --test-dir build-codex -R styio_platform_http_smoke --output-on-failure`.
For registry kernel changes, also run `python3 tests/unit/test_registry_v2.py`
and the native registry route tests in `styio_platform_native_tests`. For VM
deployment helper changes, also run `python3 tests/unit/test_registry_vm_deploy.py`.
For Postgres store changes, configure at least one local build with
`libpq-dev` available before relying on CI to cover the driver-enabled path.
For Helm or direct mTLS changes, render both default and TLS-enabled chart
variants with `helm template`.
For tool-release helper changes, include
`python3 -m unittest tests/unit/test_tool_release.py`.
For compile container changes, include native service tests and platform
control-plane contract gates in addition to the full CTest run.

## Cross-Team Dependencies

Coordinate with Control Plane for native JSON contract package changes, with upstream
`styio` for compiler semantics, and with upstream `styio-spio` for resolver or
manifest semantics. Coordinate registry route and mirror behavior with
`styio-spio` so client fetch/offline semantics remain compatible.

## Handoff / Recovery

If a platform kernel change breaks compatibility, preserve the existing
`styio-spio` client behavior and document the cutover blocker in planning docs.

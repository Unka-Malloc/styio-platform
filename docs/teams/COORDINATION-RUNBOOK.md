# Coordination Runbook

**Purpose:** Coordinate cross-team ownership for platform kernel, global control-plane, package distribution, and docs delivery changes.

**Last updated:** 2026-05-09

## Mission

Keep `styio-platform` aligned with upstream compiler and package-manager repos
while it becomes the global cloud-computing, regional deployment, and package
distribution foundation for `styio-spio`.

## Module Map

- `src/PlatformCore/` owns core package, resolver, registry-client, and toolchain support.
- `src/PlatformCore/System/` owns config loading, filesystem/process helpers, and centralized operating-system adapters.
- `src/PlatformStorage/PlatformCache/` owns shared cache and local state layout.
- `src/PlatformStorage/PlatformPersistence/` owns persisted state records, memory-backed development state, Postgres state access, migrations, and object storage access.
- `src/PlatformSecurity/PlatformCA/` owns certificate authority, trust-anchor lifecycle, and managed mTLS certificate issuance.
- `src/PlatformSecurity/PlatformClientAuth/` owns mTLS identity parsing, platform role recognition, internal role checks, and operation-level authorization policy.
- `src/PlatformSecurity/SecurityHardening/` owns registry security policy hooks and hardening extension points.
- `src/PlatformCloud/PackageRegistry/` owns package registry mirror helpers and registry v2 tooling.
- `src/PlatformCloud/DeveloperWorkspace/` owns cloud job queues, worker runtime, compile-container/workspace factories, and workspace compile stress harnesses.
- `src/PlatformService/` owns external HTTP routing, adapters, and daemon entrypoints.
- `src/SpioPlatformProtocols/` owns all Spio-to-Platform interaction payloads and serializers for compile plans, project graphs, cloud policy, and build job requests.
- `manifests/` declares business capability ownership and extraction checklists.
- `docs/governance/Platform-Workspace-Compile-Model.md` owns the Styio default target, C++/LLVM workspace, and mixed compile rule.
- `contracts/` publishes compile-plan, hosted control-plane, platform control-plane, registry control-plane, and registry v2 packages.
- `docs/registry/` owns package distribution and mirror synchronization rules.
- `docs/operations/` owns regional node deployment and recovery rules.
- `scripts/` owns server tools, docs gates, and stress harnesses.
- `docs/` owns service governance and operational policy.

## Ownership Table

| Surface | Owner |
|---------|-------|
| `src/`, `tests/`, `manifests/`, workspace compile model | Platform Kernel |
| `contracts/`, server scripts, regional nodes, mirrors | Control Plane |
| `docs/`, docs scripts | Docs Delivery |

## Review Matrix

Platform kernel changes need native tests and must preserve the Styio-default,
C++/LLVM-capable workspace model. Control-plane changes need contract or script
gates over native JSON packages. Regional node and mirror changes need explicit
authority, freshness, and recovery rules. Docs changes need docs audit and
runbook ownership updates.

The V1 cloud service is coordinated as a single-region runnable C++ kernel:
Boost.Beast/Asio networking, Postgres durable state, provider-neutral object
storage with S3 first, and mTLS service traffic. Multi-region expansion must not
precede executable compatibility gates for that kernel.

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
and record the missing platform gate in `docs/plan/`.

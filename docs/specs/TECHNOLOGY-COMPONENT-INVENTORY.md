# Technology And Component Inventory

**Purpose:** Define the required technology-stack, internal-component, open-source-component, and dependency-manifest inventory for `styio-platform`.

**Last updated:** 2026-04-24

This document is the repository-local maintenance rule for the manifest inventory audited by `styio-audit`. The canonical audit module must list the same surfaces in `for-styio-platform/module.json`; if this document and the audit manifest diverge, the change is not closed.

## Required Inventory Fields

Every audit manifest for this repository must maintain these non-empty lists:

1. `technology_stack`
2. `internal_components`
3. `open_source_components`
4. `dependency_manifests`

Missing or stale lists are audit failures. They block license, commercial-risk, ownership, and usage-boundary review because auditors cannot prove what stack and components are in scope.

## Current Inventory

Technology stack:

- C++ platform service kernel built with CMake and CTest.
- Native JSON contract packages and canonical examples.
- Python contract, registry, docs, hygiene, and stress gates.
- Bash delivery and docs scripts.
- Systemd-managed Linux VM deployment packaging for registry server nodes.
- JSON and YAML control-plane artifacts.
- TypeScript and web fixture surfaces present in the repository.
- GitHub Actions workflow automation.

Internal components:

- `PlatformService` route dispatch, daemon self-test, identity, object-store, and job lifecycle code.
- Registry control-plane and registry v2 contract packages.
- VM registry deployment package, installer, static read server, and smoke gate.
- Native contract governance, example packs, and source gates.
- Registry mirror distribution and regional node runbooks.
- Docs ownership, team runbook, repo hygiene, and delivery gate automation.
- Native and interop tests for platform service and contract compatibility.

Open-source and external components:

- CMake and CTest.
- `nlohmann_json`.
- `tomlplusplus`.
- `googletest`.
- Python standard library tooling.
- Bash shell tooling.
- OpenSSL command-line tooling for registry role-key generation.
- systemd-compatible Linux service management for VM deployment.
- GitHub Actions.

Dependency manifest surfaces:

- `CMakeLists.txt`.
- `src/CMakeLists.txt`.
- `tests/CMakeLists.txt`.
- `contracts/**/*.json`.
- `.github/workflows/*.yml`.

## Maintenance Rule

Update this document and the matching `styio-audit` project module in the same change whenever any of these occur:

1. A language, SDK, runtime, build system, CI system, package manager, contract format, or generated-code tool is added or removed.
2. A first-party service-kernel, contract, registry, mirror, regional-node, gate, or workflow boundary is added, renamed, or retired.
3. An open-source or external component is introduced, removed, vendored, promoted from fixture-only to production use, or given a new usage boundary.
4. A dependency manifest or contract package is added, removed, renamed, or moved.
5. License, Apache-2.0, commercial-authorization, subscription, membership, trial-only, or proprietary-use evidence changes.

For new external dependencies, create or update dependency usage-boundary evidence before the change can pass audit.

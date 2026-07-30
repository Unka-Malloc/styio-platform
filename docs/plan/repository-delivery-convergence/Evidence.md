# Styio Platform Owner Convergence Evidence

**Purpose:** Identify the repository-owned proof surfaces for the convergence.

**Last updated:** 2026-07-30

- Registry identifiers and routes:
  `src/PlatformCloud/PackageRegistry/ControlPlane/RegistryIdentifiers.hpp` and
  `RegistryRoutes.cpp`.
- Hosted worker command and compiler handoff:
  `src/PlatformCloud/DeveloperWorkspace/Worker.cpp` and
  `WorkerRuntimeFactory.cpp`.
- Registry service contracts: `contracts/registry-v2/v1` and
  `contracts/registry-control-plane/v1`.
- Focused behavior checks: native Platform tests, registry Python unit tests,
  hosted/control-plane contract tests, and the native source gate.

This file names stable evidence surfaces only. Test outcomes and immutable
revisions are recorded by the final Pafio ecosystem matrix.

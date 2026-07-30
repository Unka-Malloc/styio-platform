# Platform Kernel Runbook

**Purpose:** Own the Styio Platform service foundation, hosted job/worker
kernel, persistence, security, and C++ service integration boundary.

**Last updated:** 2026-07-30

## Mission

Maintain the runnable Platform kernel without duplicating Pafio project logic
or Styio compiler contracts. Platform owns hosted workspace scheduling,
registry services, job state, workers, storage, security, and service routing.

## Owned Surface

- `src/PlatformCore/Config.*`, `Core/`, and `System/` for platform config,
  shared paths/process helpers, and operating-system adapters.
- `src/PlatformCore/SourceFetch/` for bounded source checkout used by hosted
  jobs.
- `src/PlatformStorage/PlatformPersistence/` and
  `src/PlatformStorage/PlatformRecovery/` for durable state, objects, backup,
  and restore.
- `src/PlatformSecurity/` for CA, mTLS identity, authorization, external
  identity, and registry hardening.
- `src/PlatformCloud/DeveloperWorkspace/` for job queues, workers,
  compile-container bindings, and workspace factories.
- `src/PlatformCloud/PackageRegistry/` for registry control, publication,
  static read, mirrors, and release management.
- `src/PlatformService/` for HTTP adapters, route catalogs, operations, and the
  daemon.
- `contracts/hosted-control-plane/` and `contracts/platform-control-plane/` for
  Platform-owned hosted and job/worker APIs.
- `tests/native/` for the service kernel.

Pafio owns manifests, locks, resolution, project metadata, dependency sync, and
project workflow envelopes. Styio owns compile-plan consumption, compiler
diagnostics, receipts, and runtime events. Platform workers call `pafio build`
and provide the configured system Styio path through `PAFIO_STYIO_BIN`.

## Daily Workflow

Keep `PlatformService/Router.cpp` limited to dispatch. Put capability handlers
beside the state they mutate. Route environment, filesystem, child-process,
socket, and sleep behavior through `PlatformCore/System`.

Job request changes must keep the native JSON contract, queue validation,
worker argument construction, memory state, Postgres state, and focused native
tests aligned. The job request may carry source checkout, profile, workflow
flags, and target selection. Worker-pool selection remains an outer Platform
scheduling field.

## Change Classes

Kernel changes include service routing, job/worker lifecycle, compile-container
binding, source checkout, persistence, object storage, recovery, security, and
Platform-owned contract shape. Pafio project semantics or Styio compiler
payloads are upstream changes and must not be reimplemented here.

## Required Gates

Run the smallest focused native filter for the changed capability, the
corresponding native JSON contract gate, any necessary target build, and
`git diff --check`. Run the full repository regression only once after all
coordinated closures are complete.

For job/worker changes, include:

```sh
ctest --test-dir build-codex -R 'PlatformServiceJobQueueTests|PlatformCloudWorkerFactoryTests' --output-on-failure
python3 tests/interop/platform-control-plane-contract-gate.py --mode integration
python3 tests/interop/hosted-control-plane-contract-gate.py --mode integration
```

## Cross-Team Dependencies

Coordinate project metadata and workflow changes with Pafio, compiler contract
changes with Styio, and frontend hosted-route changes with Vityo. Registry
client behavior remains Pafio-owned; registry service behavior remains
Platform-owned.

## Handoff / Recovery

Use fixed repository revisions for cross-repository validation. Do not restore
removed package-manager, compiler-management, or compiler-contract copies as a
fallback. Record an upstream blocker instead of adding a compatibility shim.

# Control Plane Runbook

**Purpose:** Own global hosted workspace, native JSON platform control-plane, regional node, registry distribution, mirror sync, server-script, and cloud stress surfaces.

**Last updated:** 2026-05-02

## Mission

Keep global cloud service contracts executable and independently testable inside
`styio-platform`, including multi-region deployment, package distribution, and
mirror synchronization.

## Owned Surface

- `contracts/compile-plan/`
- `contracts/hosted-control-plane/`
- `contracts/platform-control-plane/`
- `contracts/registry-control-plane/`
- `contracts/registry-v2/`
- `docs/governance/Platform-Global-Service-Model.md`
- `docs/governance/Platform-Workspace-Compile-Model.md`
- `docs/governance/Spio-Cloud-Control-Plane-Contract.md`
- `docs/operations/Platform-Regional-Node-Runbook.md`
- `docs/registry/Platform-Mirror-Synchronization-Contract.md`
- `scripts/cloud-compile-stress.py`
- `scripts/deploy-registry-vm.sh`
- `scripts/package-registry-server.sh`
- `scripts/registry-v2-control-plane-server.py`
- `scripts/registry-v2-static-read-server.py`
- `scripts/registry-v2-vm-smoke.py`
- `src/spio_cloud_stress/`
- `tests/interop/` and `tests/unit/`

## Daily Workflow

Update native JSON contracts and examples first, run contract gates, and
refresh service runbooks in the same change. Regional node and mirror-sync
changes must describe authority, lag, replay, and failure isolation explicitly.
Platform HTTP adapter changes must keep the native JSON route package, daemon
commands, and network smoke test aligned.
Registry-management changes must keep hosted publish, verify, mirror
freshness/replay, service cache, offline-client fallback, and security policy
coverage visible in the affected docs or gates. VM deployment changes must keep
the package bundle, installer, systemd services, static read plane, and smoke
check aligned in the same change.
Registry descriptor changes must keep
`GET /api/spio-registry-control/v1/descriptor`, examples, smoke scripts, and
client trust-import docs aligned. Hosted publish changes that move registry
objects to S3 must describe the write authority, static read root URL, and
object immutability assumptions in the registry operations runbook.

## Change Classes

Control-plane changes include route shape, native JSON contract/example
packages, registry server behavior, hosted workspace envelopes, regional node
behavior, mirror freshness, package distribution, workspace target selection,
mixed Styio/C++ execution envelopes, VM deployment packaging, and cloud stress
scenarios. S3-backed registry publication and in-process mTLS termination are
control-plane changes because they affect deployment authority and service
trust boundaries.

The V1 cloud-service implementation boundary is pure C++ with Boost.Beast and
Boost.Asio, Postgres for durable state, provider-neutral object storage with S3
first, mTLS for service traffic, and a single-region runnable kernel before
multi-region promotion.

## Required Gates

Run Python unit tests, native JSON contract gate tests, example smoke checks
for touched contract packages, and mirror/regional-node validation once those
executable gates exist.
For platform HTTP adapter changes, include
`ctest --test-dir build-codex -R styio_platform_http_smoke --output-on-failure`.
For registry-control-plane changes, include
`python3 tests/interop/registry-control-plane-contract-gate.py` and
`python3 tests/interop/native-contract-source-gate.py`. For hosted registry
route changes, include the native registry route tests inside `ctest --test-dir
build-codex --output-on-failure`. For VM registry deployment changes, include
`python3 tests/unit/test_registry_vm_deploy.py`.

## Cross-Team Dependencies

Coordinate with Platform Kernel when contracts depend on C++ payload shape.
Coordinate with Docs Delivery whenever service ownership or runbooks change.
Coordinate with upstream `styio-spio` when a platform distribution contract
changes local package-manager behavior.

## Handoff / Recovery

If a server contract cannot move yet, keep a temporary `styio-spio` reference
and record the platform-side blocker in `docs/planning/Platform-Migration-Plan.md`.
If a regional node or mirror sync contract is not executable yet, keep the
documented contract here and record the missing gate before claiming cutover.

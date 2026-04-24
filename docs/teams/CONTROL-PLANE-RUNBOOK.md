# Control Plane Runbook

**Purpose:** Own global hosted workspace, native JSON platform control-plane, regional node, registry distribution, mirror sync, server-script, and cloud stress surfaces.

**Last updated:** 2026-04-24

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
- `scripts/registry-v2-control-plane-server.py`
- `src/spio_cloud_stress/`
- `tests/interop/` and `tests/unit/`

## Daily Workflow

Update native JSON contracts and examples first, run contract gates, and
refresh service runbooks in the same change. Regional node and mirror-sync
changes must describe authority, lag, replay, and failure isolation explicitly.
Registry-management changes must keep hosted publish, verify, mirror
freshness/replay, service cache, offline-client fallback, and security policy
coverage visible in the affected docs or gates.

## Change Classes

Control-plane changes include route shape, native JSON contract/example
packages, registry server behavior, hosted workspace envelopes, regional node
behavior, mirror freshness, package distribution, workspace target selection,
mixed Styio/C++ execution envelopes, and cloud stress scenarios.

The V1 cloud-service implementation boundary is pure C++ with Boost.Beast and
Boost.Asio, Postgres for durable state, provider-neutral object storage with S3
first, mTLS for service traffic, and a single-region runnable kernel before
multi-region promotion.

## Required Gates

Run Python unit tests, native JSON contract gate tests, example smoke checks
for touched contract packages, and mirror/regional-node validation once those
executable gates exist.
For registry-control-plane changes, include
`python3 tests/interop/registry-control-plane-contract-gate.py` and
`python3 tests/interop/native-contract-source-gate.py`.

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

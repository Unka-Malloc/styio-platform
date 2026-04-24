# Control Plane Runbook

**Purpose:** Own global hosted workspace, regional node, registry distribution, mirror sync, server-script, and cloud stress surfaces.

**Last updated:** 2026-04-24

## Mission

Keep global cloud service contracts executable and independently testable inside
`styio-platform`, including multi-region deployment, package distribution, and
mirror synchronization.

## Owned Surface

- `contracts/compile-plan/`
- `contracts/hosted-control-plane/`
- `contracts/registry-control-plane/`
- `contracts/registry-v2/`
- `docs/governance/Platform-Global-Service-Model.md`
- `docs/governance/Platform-Workspace-Compile-Model.md`
- `docs/operations/Platform-Regional-Node-Runbook.md`
- `docs/registry/Platform-Mirror-Synchronization-Contract.md`
- `scripts/cloud-compile-stress.py`
- `scripts/export_hosted_control_plane_api.py`
- `scripts/registry-v2-control-plane-server.py`
- `src/spio_cloud_stress/`
- `tests/interop/` and `tests/unit/`

## Daily Workflow

Update machine-readable contracts first, regenerate derived artifacts, run
contract gates, and refresh service runbooks in the same change. Regional node
and mirror-sync changes must describe authority, lag, replay, and failure
isolation explicitly.

## Change Classes

Control-plane changes include route shape, OpenAPI/Arazzo output, registry
server behavior, hosted workspace envelopes, regional node behavior, mirror
freshness, package distribution, workspace target selection, mixed Styio/C++
execution envelopes, and cloud stress scenarios.

## Required Gates

Run Python unit tests, contract gate tests, generated-artifact checks for
touched API packages, and mirror/regional-node validation once those executable
gates exist.

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

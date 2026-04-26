# Spio Registry Control-Plane Contract

**Purpose:** Freeze the HTTP control-plane contract used by backend services and automation to operate a `spio` registry `v2` root independently of the static read-plane contract.

**Last updated:** 2026-04-24

## Source Of Truth

The authoritative machine contract lives under:

- [`../../contracts/registry-control-plane/v1/registry-control-plane.contract.json`](../../contracts/registry-control-plane/v1/registry-control-plane.contract.json)
- [`../../contracts/registry-control-plane/v1/registry-control-plane.examples.json`](../../contracts/registry-control-plane/v1/registry-control-plane.examples.json)

Human-readable docs explain those files. They do not replace them.

## Scope

`v1` currently freezes these control-plane operations:

1. `GET /api/spio-registry-control/v1/status`
2. `POST /api/spio-registry-control/v1/publish`
3. `POST /api/spio-registry-control/v1/verify`

This contract owns:

- control-plane status and readiness discovery
- publish requests that commit source packages into a `v2` root
- verification requests for the static read plane

This contract does not own:

- the immutable read-plane object layout itself
- browser-facing frontend deployment flows
- future hosted auth and tenancy policy

Those remain in:

- [`Spio-Registry-V2-Protocol.md`](./Spio-Registry-V2-Protocol.md)
- [`Spio-Registry-V2-Publish-Control-Plane.md`](./Spio-Registry-V2-Publish-Control-Plane.md)

## Repository Boundary

`styio-platform` owns the hosted registry and mirror service side for this
contract. `styio-spio` consumes the same native JSON package as a local
package-manager compatibility boundary. Service implementations must preserve
the shared status, publish, and verify envelopes while keeping mirror
freshness/replay in platform mirror docs and offline cache behavior in
`styio-spio` client docs.

## Current Implementation Status

The tracked open-source repository now ships a local HTTP implementation:

- [`../../scripts/registry-v2-control-plane-server.py`](../../scripts/registry-v2-control-plane-server.py)

That server binds:

- one local registry root
- one role-key directory
- one `spio` binary for dry-run publish preparation

It is the current executable reference for the contract package. It does not yet represent the final hosted multi-tenant service shape.

## Gate Rule

Registry control-plane compatibility is validated from the native JSON contract
and example package. Any route or envelope change must update both JSON files,
this governance page, and the owning runbook before claiming the service
boundary is stable. Generated third-party API-description artifacts are not
accepted as contract source.

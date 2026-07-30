# Platform Hosted Control-Plane Contract

**Purpose:** Freeze the frontend/backend HTTP contract used by hosted workspaces so `vityo-nightly`, the platform control console, and backend services can develop independently against a versioned native JSON package.

**Last updated:** 2026-07-30

## Source Of Truth

The authoritative machine contract lives under:

- [`contracts/hosted-control-plane/v1/hosted-control-plane.contract.json`](../../contracts/hosted-control-plane/v1/hosted-control-plane.contract.json)
- [`contracts/hosted-control-plane/v1/hosted-control-plane.examples.json`](../../contracts/hosted-control-plane/v1/hosted-control-plane.examples.json)

Human-readable docs must explain and reference those files, not replace them.

## Engineering Package

`v1` ships as a native JSON interface package:

1. `hosted-control-plane.contract.json`
   Local compatibility contract catalog used by native JSON shape gates.
2. `hosted-control-plane.examples.json`
   Canonical request/success/failure examples.

## Scope

This contract owns:

- hosted workspace open and project-graph routes
- dependency materialization routes
- execution routes
- deployment routes
- shared success and failure envelopes

This contract does not own:

- internal worker scheduling
- registry read/write repository layout
- compiler-private payloads behind `styio`

Those stay in their existing contracts.

## Route Families

`v1` freezes these operations:

1. `POST /api/styio-hosted/v1/workspaces/open`
2. `GET /api/styio-hosted/v1/workspaces/{workspace_id}/project-graph`
3. `POST /api/styio-hosted/v1/workspaces/{workspace_id}/dependencies/sync`
4. `POST /api/styio-hosted/v1/workspaces/{workspace_id}/dependencies/vendor`
5. `POST /api/styio-hosted/v1/workspaces/{workspace_id}/execution/run`
6. `POST /api/styio-hosted/v1/workspaces/{workspace_id}/execution/build`
7. `POST /api/styio-hosted/v1/workspaces/{workspace_id}/execution/test`
8. `POST /api/styio-hosted/v1/workspaces/{workspace_id}/deployment/pack`
9. `POST /api/styio-hosted/v1/workspaces/{workspace_id}/deployment/preflight`
10. `POST /api/styio-hosted/v1/workspaces/{workspace_id}/deployment/publish`

## Envelope Rules

- `open` and `project-graph` publish a hosted project envelope with `payload` and `workspace`.
- dependency, execution, and deployment routes publish command envelopes with `returncode`, `message`, `stdout`, `stderr`, and route-specific `payload` or `error_payload`.
- execution payloads must keep `diagnostics` and `runtime_events` structured.
- deployment payloads must preserve `archive_path` so frontend flows can show durable artifacts.
- domain-level failures are modeled inside the documented JSON envelope; clients must inspect `returncode` instead of relying on page-local heuristics.

Styio is a system-provided dependency. This contract has no compiler install,
use, pin, channel, or managed-cache operations. Hosted workers invoke Pafio
with an explicitly configured Styio executable.

## Workflow Rule

Any frontend or control-console workflow that spans multiple operations must be represented in the native JSON contract/examples package once it becomes part of the expected product path. Route docs alone are not enough for independent frontend/backend development.

## Testing Rule

Every published operation must keep:

- unit coverage for contract invariants
- integration coverage for client/server request semantics
- regression coverage for the frozen method/path table
- smoke coverage for every example request/response pair
- fuzz coverage for malformed payload rejection

And the package as a whole must keep:

- native JSON contract shape gates
- canonical example smoke gates
- workflow coverage for multi-operation product paths

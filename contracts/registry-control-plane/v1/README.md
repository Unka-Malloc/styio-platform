# registry-control-plane v1

**Purpose:** Define the first versioned HTTP control-plane package for operating a `spio` registry `v2` root over a backend service boundary.

**Last updated:** 2026-04-21

## Source Of Truth

- `openapi.json` is the canonical standards-based API description.
- `workflows.arazzo.json` is the workflow description for status, publish, and verify flows.
- `registry-control-plane.contract.json` is the compatibility catalog used by local gates.
- `registry-control-plane.examples.json` is the canonical example pack.
- `redocly.yaml` freezes the OpenAPI lint profile used by contract gates.

## Stability Rules

- Additive optional request fields are allowed within `v1`.
- Renaming operations, changing required fields, or changing envelope semantics requires `v2`.
- Clients must treat undocumented fields as non-existent.
- Services must preserve the published method, path, and envelope spelling exactly.

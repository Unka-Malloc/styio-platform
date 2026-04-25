# registry-control-plane v1

**Purpose:** Define the first native JSON HTTP control-plane package for operating a `spio` registry `v2` root over a backend service boundary.

**Last updated:** 2026-04-24

## Source Of Truth

- `registry-control-plane.contract.json` is the canonical status, publish, and verify operation catalog.
- `registry-control-plane.examples.json` is the canonical request, success, and failure example pack.
- Human-readable registry docs stay secondary to this package.

## Package Workflow

1. Edit `registry-control-plane.contract.json` and `registry-control-plane.examples.json`.
2. Update registry governance docs when route families or envelope rules change.
3. Verify the native JSON package with the contract and example gates before claiming compatibility.

## Stability Rules

- Additive optional request fields are allowed within `v1`.
- Renaming operations, changing required fields, or changing envelope semantics requires `v2`.
- Clients must treat undocumented fields as non-existent.
- Services must preserve the published method, path, and envelope spelling exactly.

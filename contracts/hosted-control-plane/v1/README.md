# hosted-control-plane v1

**Purpose:** Define the native JSON HTTP contract for Platform-hosted workspaces consumed by `vityo-nightly`.

**Last updated:** 2026-07-30

## Source Of Truth

- `hosted-control-plane.contract.json` is the canonical operation, schema, and envelope catalog.
- `hosted-control-plane.examples.json` is the canonical request, success, and failure example pack.
- Human-readable governance notes stay secondary to this package.

## Package Workflow

1. Edit `hosted-control-plane.contract.json` and `hosted-control-plane.examples.json`.
2. Update this README and owning governance docs when route families or envelope rules change.
3. Verify the native JSON package with the contract and example gates before claiming compatibility.

## Stability Rules

- `v1` starts at the Pafio ownership cutover and contains no managed compiler
  routes or compiler-selection state.
- Additive optional fields are allowed within `v1`.
- Removing an operation, renaming a field, changing a required field, or changing an enum meaning requires `v2`.
- Frontend clients must treat undocumented fields as non-existent.
- Backend services must preserve the published method, path, and envelope spelling exactly.

Pafio owns dependency and project workflow behavior invoked behind hosted
routes. Styio is system-provided and owns compiler machine contracts; Platform
does not install, select, pin, cache, or describe compiler distributions.

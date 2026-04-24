# hosted-control-plane v1

**Purpose:** Define the first native JSON HTTP contract package for the repo-hosted and cloud-hosted workspace API consumed by `styio-view` and any future `spio` control console frontend.

**Last updated:** 2026-04-24

## Source Of Truth

- `hosted-control-plane.contract.json` is the canonical operation, schema, and envelope catalog.
- `hosted-control-plane.examples.json` is the canonical request, success, and failure example pack.
- Human-readable governance notes stay secondary to this package.

## Package Workflow

1. Edit `hosted-control-plane.contract.json` and `hosted-control-plane.examples.json`.
2. Update this README and owning governance docs when route families or envelope rules change.
3. Verify the native JSON package with the contract and example gates before claiming compatibility.

## Stability Rules

- Additive optional fields are allowed within `v1`.
- Removing an operation, renaming a field, changing a required field, or changing an enum meaning requires `v2`.
- Frontend clients must treat undocumented fields as non-existent.
- Backend services must preserve the published method, path, and envelope spelling exactly.

# hosted-control-plane v1

**Purpose:** Define the first versioned HTTP contract package for the repo-hosted and cloud-hosted workspace API consumed by `styio-view` and any future `spio` control console frontend.

**Last updated:** 2026-04-21

## Source Of Truth

- `openapi.json` is the canonical standards-based API document for frontend/backend independent development.
- `workflows.arazzo.json` is the canonical workflow document for multi-operation frontend/backend flows.
- `hosted-control-plane.contract.json` is the compatibility contract catalog used by local shape gates and derived-artifact generation.
- `hosted-control-plane.examples.json` is the canonical example pack used by tests, generated artifacts, and docs.
- `redocly.yaml` freezes the OpenAPI lint profile used by contract gates.
- Human-readable governance notes stay secondary to this package.

## Package Workflow

1. Edit `hosted-control-plane.contract.json` and `hosted-control-plane.examples.json`.
2. Regenerate the standards-based artifacts:

```bash
python3 scripts/export_hosted_control_plane_api.py --write
```

3. Verify drift and lint:

```bash
python3 scripts/export_hosted_control_plane_api.py --check
npx --yes @redocly/cli lint contracts/hosted-control-plane/v1/openapi.json --config contracts/hosted-control-plane/v1/redocly.yaml
npx --yes @redocly/cli lint contracts/hosted-control-plane/v1/workflows.arazzo.json
```

## Stability Rules

- Additive optional fields are allowed within `v1`.
- Removing an operation, renaming a field, changing a required field, or changing an enum meaning requires `v2`.
- Frontend clients must treat undocumented fields as non-existent.
- Backend services must preserve the published method, path, and envelope spelling exactly.

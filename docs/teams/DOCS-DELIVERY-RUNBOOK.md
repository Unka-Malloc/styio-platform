# Docs Delivery Runbook

**Purpose:** Own platform documentation structure, generated indexes, and docs gate automation.

**Last updated:** 2026-04-24

## Mission

Keep documentation governance aligned with the Styio ecosystem while making
`styio-platform` the global cloud-service and package-distribution foundation.

## Owned Surface

- `README.md`
- `LICENSE`
- `LICENSE-POLICY.md`
- `DEPENDENCY-USAGE.md`
- `docs/`
- `scripts/audit-gate.sh`
- `scripts/docs-audit.py`
- `scripts/docs-index.py`
- `scripts/docs-lifecycle.py`
- `scripts/repo-hygiene-gate.py`
- `scripts/team-docs-gate.py`
- `scripts/docs-gate.sh`
- `scripts/delivery-gate.sh`

## Daily Workflow

Refresh indexes after docs-tree changes, validate runbook shape, keep
`DOC-STATS.md` synchronized with runbook content, and keep Apache-2.0 license,
source-distribution policy, and dependency usage-boundary evidence aligned with
`styio-audit`. Keep `docs/specs/TECHNOLOGY-COMPONENT-INVENTORY.md` aligned
with `styio-audit` whenever the technology stack, internal components,
open-source components, dependency manifests, Apache-2.0 evidence, or commercial-risk
boundaries change. Registry-management docs must explicitly cover publish,
verify, mirror freshness/replay, offline client fallback, service/client cache
separation, and security boundaries before docs/audit closure is claimed.

## Change Classes

Docs delivery changes include collection structure, generated indexes,
runbooks, gate scripts, post-push workflow specs, native JSON contract
governance docs, technology/component inventory docs, regional-node docs, and
mirror sync docs, including minimum registry-management audit coverage.

## Required Gates

Run `python3 scripts/docs-index.py --write`, `python3 scripts/docs-audit.py`,
`./scripts/audit-gate.sh`, and `python3 scripts/repo-hygiene-gate.py --mode
tracked`.

## Cross-Team Dependencies

Coordinate with Platform Kernel and Control Plane when documentation changes
represent code, global service, package distribution, or mirror ownership
changes.
For cloud-service governance changes, verify that docs describe the native JSON
contract/examples/gates source of truth and the V1 C++ service target
consistently.

## Handoff / Recovery

If docs audit fails, update the owning runbook first, refresh generated indexes,
then rerun the docs gate.

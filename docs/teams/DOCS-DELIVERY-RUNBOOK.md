# Docs Delivery Runbook

**Purpose:** Own platform documentation structure, generated indexes, and docs gate automation.

**Last updated:** 2026-04-28

## Mission

Keep documentation governance aligned with the Styio ecosystem while making
`styio-platform` the global cloud-service and package-distribution foundation.

## Owned Surface

- `README.md`
- `LICENSE`
- `LICENSE-POLICY.md`
- `DEPENDENCY-USAGE.md`
- `.github/workflows/local-ci-gate.yml`
- `docs/`
- `docs/specs/TECHNOLOGY-COMPONENT-INVENTORY.md`
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
`styio-audit`. Keep `local-ci-gate` covering the platform repository's own
delivery floor for pull requests and managed branch pushes; it is not the
shared upstream `styio-ci-gate` ecosystem resource gate. Keep
`docs/specs/TECHNOLOGY-COMPONENT-INVENTORY.md` aligned with `styio-audit`
whenever the technology stack, internal components, open-source components,
dependency manifests, Apache-2.0 evidence, or commercial-risk boundaries
change. Keep external `styio-audit` execution wired through the repository
audit gate and dedicated GitHub Actions workflow whenever audit policy or
cross-repo CI ownership changes. Generated indexes for empty documentation
collections must derive `Last updated` from the collection `README.md` instead
of the wall clock, so `--check` stays deterministic across days. The local
audit gate must discover the sibling `styio-audit` checkout used by this
workspace before falling back to PATH. Registry-management docs must explicitly
cover publish, verify, mirror freshness/replay, offline client fallback,
service/client cache separation, VM one-command deployment, and security
boundaries before docs/audit closure is claimed.

## Change Classes

Docs delivery changes include collection structure, generated indexes,
runbooks, gate scripts, post-push workflow specs, native JSON contract
governance docs, technology/component inventory docs, regional-node docs, VM
registry deployment docs, and mirror sync docs, including minimum
registry-management audit coverage.

## Required Gates

Run `python3 scripts/docs-index.py --write`, `python3 scripts/docs-audit.py`,
`./scripts/audit-gate.sh`, `python3 scripts/repo-hygiene-gate.py --mode
tracked`, and `./scripts/delivery-gate.sh --mode checkpoint --skip-audit`.

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

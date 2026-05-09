# Docs Delivery Runbook

**Purpose:** Own platform documentation structure, generated indexes, and docs gate automation.

**Last updated:** 2026-05-09

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
boundaries change. Keep external `styio-audit` execution wired through the
repository audit gate, Tekton pipeline, and hosted status-check compatibility
wrappers whenever audit policy or cross-repo CI ownership changes. Maintain
GitHub merge gates through Rulesets rather than legacy classic branch
protection while keeping Tekton as the open CI/CD source of truth; audit
effective branch rules when required status-check governance changes.
Registry-management docs must explicitly cover publish,
verify, mirror freshness/replay, offline client fallback, service/client cache
separation, VM one-command deployment, and security boundaries before
docs/audit closure is claimed. When docs index behavior changes, keep empty
collections anchored to their README `Last updated` value instead of allowing
the generated date to drift without a source-document change.
When platform README or governance docs describe compile container behavior,
keep user binding, workspace hot-switch semantics, and worker environment
variables consistent with the native JSON contract examples.

## Change Classes

Docs delivery changes include collection structure, generated indexes,
runbooks, gate scripts, post-push workflow specs, native JSON contract
governance docs, technology/component inventory docs, regional-node docs, VM
registry deployment docs, and mirror sync docs, including minimum
registry-management audit coverage.
S3 registry, mTLS, and Helm deployment docs are docs-delivery changes when they
alter tracked install commands, values, secret names, generated indexes, or
audit-gate discovery.
Compile container documentation changes are docs-delivery changes when they
alter README setup guidance, governance rules, security boundaries, generated
indexes, or team runbook ownership.

## Required Gates

Run `python3 scripts/docs-index.py --write`, `python3 scripts/docs-audit.py`,
`./scripts/audit-gate.sh`, and `python3 scripts/repo-hygiene-gate.py --mode
tracked`.
For Helm-facing docs changes, also render `helm template` with default values
and with TLS/mTLS values enabled.
For control-plane documentation changes, also rerun the relevant native JSON
contract gate when examples or route terminology changed.

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

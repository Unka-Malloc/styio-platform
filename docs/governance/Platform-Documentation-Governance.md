# Platform Documentation Governance

**Purpose:** Define how styio-platform governs documentation ownership, source-of-truth boundaries, review gates, and cross-repository handoff for the Styio ecosystem.

**Last updated:** 2026-05-09

## Scope

Documentation governance is a platform capability, not an informal publishing
habit. The platform owns the source-of-truth map for contracts, manifests,
generated indexes, and team runbooks so documentation changes can be planned
and reviewed with the same discipline as code changes.

## Source Of Truth

The authoritative documentation inputs are:

- native JSON contracts under `contracts/`
- business capability manifests under `manifests/`
- generated `docs/**/INDEX.md` files
- team runbooks under `docs/teams/`

Human-readable docs may explain these artifacts, but they must not replace the
machine-readable contract or manifest when one exists.

## Collections

The platform treats each documentation collection as an owned governance
surface:

| Collection | Owner | Role |
|------------|-------|------|
| `docs/governance/` | Control Plane | Normative service and boundary rules |
| `docs/registry/` | Control Plane | Package registry and mirror contracts |
| `docs/operations/` | Control Plane | Deployment and operations runbooks |
| `docs/security/` | Platform Security | Trust-boundary documentation |
| `docs/specs/` | Docs Delivery | Repository-wide process specifications |
| `docs/adr/` | Architecture | Decision records |
| `docs/plan/` | Platform Kernel | Roadmap items and migration plans |
| `docs/external/` | Docs Delivery | Downstream handoff material |
| `docs/assets/` | Docs Delivery | Workflow templates and reusable gate docs |
| `docs/teams/` | Docs Delivery | Ownership and recovery runbooks |
| `docs/audit/` | Docs Delivery | Public audit summaries |

## Change Planning

The control plane exposes documentation governance through:

- `GET /api/styio-platform/v1/docs/governance`
- `POST /api/styio-platform/v1/docs/change-plan`

The change planner accepts repository-relative paths and returns impacted
collections, required team runbooks, and required gates. It intentionally plans
the review surface; it does not mutate files or run gates by itself.

## Ecosystem Boundary

The Styio ecosystem keeps documentation ownership close to the product surface:

| Repository | Documentation focus |
|------------|---------------------|
| `styio` | Language, compiler, and standard library |
| `styio-spio` | CLI, package resolution, and registry client behavior |
| `vityo-nightly` | Hosted workspace UX and console flows |
| `styio-platform` | Contracts, registry, cloud workspaces, operations |
| `styio-community` | Tutorials, examples, and public guides |

All repositories use `stable` as the only long-lived branch. Documentation
release planning follows that branch policy and may use immutable tags as
release references.

## Gates

Documentation closure requires the gate set returned by the change planner.
At minimum, tracked documentation changes must keep generated indexes fresh,
pass docs audit, preserve team runbook ownership, and keep `git diff --check`
clean. Contract-facing documentation must also update native JSON examples and
run the relevant contract gate before claiming release readiness.

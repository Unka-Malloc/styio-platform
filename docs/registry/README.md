# Registry

**Purpose:** Own package repository distribution, registry server, registry control-plane, and mirror synchronization documentation for platform-hosted package services.

**Last updated:** 2026-07-30

## Scope

This collection covers server-side registry control planes, deployment
baselines, global package distribution, regional mirrors, and cross-network
sync. Client package resolution, offline cache use, and publish command UX
remain Pafio responsibilities.
The VM deployment bundle is owned by
[../operations/Spio-Registry-Server-Runbook.md](../operations/Spio-Registry-Server-Runbook.md)
and must remain compatible with the same control-plane and static read-plane
contracts.

## Minimum Measurable Coverage

| Capability | `styio-platform` coverage |
|------------|---------------------------|
| Publish | Hosted write/control services accept authorized publish requests and commit immutable registry `v2` releases through the shared registry-control-plane package. |
| Verify | Service gates validate static-root integrity and the shared status/publish/verify envelope examples. |
| Mirror | Regional and cross-network mirrors publish freshness, replay, lag, and failure-isolation state before serving read traffic. |
| Offline | Platform availability must not be required for Pafio projects that already have local packages, caches, vendored snapshots, or import bundles. |
| Cache | Shared service caches, object storage, and mirror replicas are service-side; client `PAFIO_HOME` cache semantics stay upstream in Pafio. |
| Security | Auth, namespace ownership, mTLS, write-origin policy, and audit records are service responsibilities; client docs expose only redacted hook boundaries. |

## Maintenance Rule

Keep server-side publish, verify, mirror, cache, and security semantics here.
Keep local package-manager CLI grammar, offline resolution, and import/export
rules in Pafio. Native JSON contract/example packages are the only
service contract source of truth.

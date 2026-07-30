# Coordination Runbook

**Purpose:** Coordinate Platform ownership with Pafio, Styio, Vityo, registry,
and documentation delivery.

**Last updated:** 2026-07-30

## Mission

Keep Styio Platform the sole owner of hosted workspaces, cloud jobs, workers,
registry services, and regional operations while consuming project and compiler
contracts from their producers.

## Module Map

- `src/PlatformCore/` owns platform config, system adapters, common
  paths/process helpers, and hosted source fetch.
- `src/PlatformStorage/` owns persistence, object storage, and recovery.
- `src/PlatformSecurity/` owns platform trust and authorization.
- `src/PlatformCloud/PackageRegistry/` owns registry service behavior.
- `src/PlatformCloud/DeveloperWorkspace/` owns jobs, workers, compile
  containers, and hosted workspace execution.
- `src/PlatformService/` owns HTTP routing, adapters, and daemon entrypoints.
- `contracts/hosted-control-plane/` and `contracts/platform-control-plane/`
  publish Platform-owned APIs.
- `manifests/` declares Platform business capability ownership.

Pafio owns manifest, lock, resolution, metadata, sync, vendor, pack, publish
client, and project workflows. Styio owns compiler machine contracts,
compile-plan consumption, diagnostics, receipts, and runtime events. Vityo
adapts those producer contracts and Platform hosted APIs into frontend models.

## Ownership Table

| Surface | Owner |
|---------|-------|
| Hosted workspace, jobs, workers, registry service | Styio Platform |
| Project metadata and local workflows | Pafio |
| Compiler and language-service contracts | Styio |
| Editor presentation and adapters | Vityo |
| Platform operational documentation | Docs Delivery |

## Review Matrix

Platform kernel changes need focused native tests. Public route changes need
the matching contract/example gate. Registry changes need registry contracts
and security review. Cross-owner changes need a fixed revision matrix; no
consumer may inspect another product's private home or cache.

## Escalation Rules

Send project-model or package-manager changes to Pafio. Send compiler behavior
or machine-contract changes to Styio. Send frontend view-model changes to
Vityo. Keep only Platform scheduling, hosted lifecycle, registry, and worker
semantics in this repository.

## Checkpoint Policy

Close one independently testable capability at a time. Run focused tests after
each closure and one full regression only after all coordinated changes are
implemented and independently reviewed.

## Release / Cutover Gates

The no-compatibility cutover requires fixed Styio, Pafio, Platform, and Vityo
revisions, passing owner-contract checks, and one coordinated nightly release
window.

## Handoff / Recovery

Do not restore removed owner copies when integration fails. Report the
producer revision or contract mismatch, preserve the clean ownership boundary,
and rerun the fixed-revision matrix after the producer is corrected.

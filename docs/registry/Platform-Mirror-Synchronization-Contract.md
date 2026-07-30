# Platform Mirror Synchronization Contract

**Purpose:** Define the package repository mirror synchronization contract for regional and cross-network `styio-platform` deployments.

**Last updated:** 2026-04-29

## Scope

Mirror synchronization covers package metadata, immutable package objects,
registry v2 indexes, transparency materials, and mirror freshness status.

## Rules

- Authoritative writes are accepted only by configured write origins or their
  explicit forwarding control planes.
- Mirror nodes may serve read traffic only after publishing freshness metadata.
- Mirror lag is a first-class state; clients must not infer freshness from
  successful HTTP reads alone.
- Immutable package objects may replicate independently from mutable indexes,
  but indexes must not reference missing required objects on a promoted mirror.
- Failed mirror sync must be observable and replayable.

The Kubernetes V1 mirror sync copies filesystem-backed registry roots in this
order: `config.json`, `trust/`, immutable `artifacts/`, mutable `index/`, then
`log/`. After a sync pass, `styio-platformd --sync-mirror-once` records the
mirror id, origin, freshness state, and replay cursor in Postgres. The mirror
read plane serves only the copied PVC content; clients must still check
freshness through the platform control plane.

## Client Contract

Pafio clients may select a mirror for package reads, but local offline
packages and explicitly imported packages remain valid even when no platform or
mirror endpoint is reachable.

## Audit Coverage

Mirror changes are measurable only when the docs or gates identify:

- authoritative write origin and accepted forwarding path
- freshness cursor and replay behavior
- immutable object and mutable index replication order
- cache boundary between service replicas and Pafio-owned client state
- security boundary for mirror promotion, service credentials, and public reads
- fallback behavior for offline-capable Pafio clients

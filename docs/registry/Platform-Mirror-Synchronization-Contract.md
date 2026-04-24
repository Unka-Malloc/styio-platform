# Platform Mirror Synchronization Contract

**Purpose:** Define the package repository mirror synchronization contract for regional and cross-network `styio-platform` deployments.

**Last updated:** 2026-04-24

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

## Client Contract

`styio-spio` clients may select a mirror for package reads, but local offline
packages and explicitly imported packages remain valid even when no platform or
mirror endpoint is reachable.

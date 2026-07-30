# Styio, Pafio, and Platform Alignment

**Purpose:** Record the producer boundaries consumed by Styio Platform.

**Last updated:** 2026-07-30

## Alignment Rules

- Compiler semantics and compile-plan execution capability come from `styio`.
- Package manifests, resolver behavior, metadata, lockfiles, pack, publish, and
  local CLI UX come from Pafio.
- Hosted workspaces, job scheduling, workers, registry control planes, and
  server-side service runbooks live in `styio-platform`.
- Contract changes that affect a client must update the upstream client docs before platform docs claim closure.

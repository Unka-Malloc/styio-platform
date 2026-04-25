# Styio Spio Downstream Alignment

**Purpose:** Record how `styio-platform` stays downstream of both `styio` and `styio-spio` after the server migration.

**Last updated:** 2026-04-24

## Alignment Rules

- Compiler semantics and compile-plan execution capability come from `styio`.
- Package manifests, resolver behavior, lockfiles, pack, publish, and local CLI UX come from `styio-spio`.
- Hosted workspaces, cloud execution policy, registry control planes, and server-side service runbooks live in `styio-platform`.
- Contract changes that affect a client must update the upstream client docs before platform docs claim closure.

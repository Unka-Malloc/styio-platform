# styio-platform Documentation

**Purpose:** Define the documentation collections, gates, and ownership model for the global Styio cloud-service and package-distribution platform.

**Last updated:** 2026-04-24

## Scope

`styio-platform` documentation is downstream of the compiler contract in Styio
and the package-manager client contract in Pafio. Platform docs own
hosted workspaces, Pafio workflow execution, registry control-plane
servers, package distribution, multi-region deployment nodes, mirror
synchronization, workspace compile semantics, native C++/LLVM fallback, and
cloud-service extensibility.

## Required Gates

- `python3 scripts/docs-index.py --write` refreshes generated indexes.
- `python3 scripts/docs-lifecycle.py validate` validates lifecycle metadata.
- `python3 scripts/docs-audit.py` checks collection metadata and ownership.
- `python3 scripts/repo-hygiene-gate.py --mode tracked` checks repository hygiene.

## Collections

Collection `README.md` files describe local scope. Generated `INDEX.md` files
are inventories and should be refreshed by script after docs-tree changes.

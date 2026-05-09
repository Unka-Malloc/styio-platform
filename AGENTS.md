# Agent Maintenance Guide

Use the source tree as the primary map. Docs explain ownership, gates, and
non-obvious constraints; they should not restate implementation details that
are visible in code, tests, contracts, or manifests.

## Source Map

- `src/PlatformCore/`: config, manifests, resolver, source fetch, registry
  client, toolchain state, and OS adapters.
- `src/PlatformStorage/`: persistence, object storage, cache layout, recovery.
- `src/PlatformSecurity/`: CA, mTLS identity, authorization, external identity,
  hardening.
- `src/PlatformCloud/PackageRegistry/`: registry control plane, publication
  builder, static read plane, mirror sync, release channels. Control-plane
  handlers are split by package, owner/token, and repository/distribution
  behavior; helper headers are named by registry concern.
- `src/PlatformCloud/DeveloperWorkspace/`: job queue, workers, compile
  containers, workspace factories.
- `src/PlatformCloud/DocumentationGovernance/`: docs collections, owners, gate
  planning.
- `src/PlatformCloud/EcosystemManagement/`: Styio ecosystem repository and
  release-plan model.
- `src/PlatformService/`: HTTP adapter, route catalog, router, daemon, ops
  helpers.
- `src/SpioPlatformProtocols/`: Spio-to-Platform payloads and serializers.

## Source Of Truth

- Public API shape: `contracts/**`.
- Business ownership: `manifests/*.yaml`.
- Behavior: `tests/`.
- Build graph: `src/CMakeLists.txt`.
- Operational constraints: `docs/operations/` and `docs/teams/`.
- Design constraints that are not obvious from code: `docs/governance/`.

## Change Rules

- Prefer moving behavior into the owning module over adding Router logic.
- Keep route catalogs in `PlatformService/RouteCatalog.*` or the owning
  capability route file.
- Keep `PlatformService/Router.cpp` as dispatch only; capability handlers live
  under their owning `PlatformCloud/*` or `PlatformService/PlatformOps` path.
- Do not duplicate contract fields in prose docs; link the contract and record
  only the boundary or invariant.
- Regenerate `docs/**/INDEX.md` after docs-tree changes.
- Update team runbooks and `docs/teams/DOC-STATS.md` when ownership surfaces
  change.

## C++ Comments

Follow the Google C++ Style Guide comment model for maintained C++ code:

- Prefer `//` comments. Use block comments only when the local style already
  requires them.
- Comment declarations that define a boundary: classes, structs, public
  functions, and headers that group multiple related abstractions.
- Write descriptive comments for the next maintainer. Explain the contract,
  invariant, failure mode, ownership rule, or compatibility reason.
- Do not restate implementation steps that are visible from names and control
  flow.
- Keep implementation comments close to tricky, non-obvious, or security-
  sensitive code.
- Write TODOs as `TODO(owner): detail`, with an issue or owner that can close
  the work. Do not add anonymous TODO/FIXME notes.
- Use complete sentences with normal punctuation.

## Local Gates

```sh
cmake --build build-codex -j 4
ctest --test-dir build-codex --output-on-failure
bash scripts/docs-gate.sh
python3 scripts/repo-hygiene-gate.py --mode working
git diff --check
```

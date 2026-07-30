# styio-platform Source

This source tree contains the Platform service kernel. Source ownership is
grouped by platform capability.

- `PlatformCore/` owns platform config, shared paths/process helpers, source
  fetching for hosted jobs, and operating-system adapters.
- `PlatformCore/System/` owns the system layer: config loading,
  filesystem/process helpers, and centralized operating-system adapters.
- `PlatformStorage/` owns the storage layer:
  `PlatformPersistence/` contains persisted state records, the memory-backed
  development state store, Postgres migrations/state access, and object storage
  access for S3, filesystem, or memory-backed object stores.
  `PlatformRecovery/` owns filesystem snapshot, backup, and restore primitives.
- `PlatformSecurity/` owns security ability:
  `ExternalIdentity/` normalizes Microsoft, Google, Apple, and Telegram
  identity assertions into platform actors, `PlatformCA/` contains certificate
  authority, trust-anchor lifecycle, and managed mTLS certificate issuance,
  `PlatformClientAuth/` contains mTLS identity parsing and operation-level
  authorization policy, and
  `SecurityHardening/` contains registry read/write security hooks and
  hardening extension points.
- `PlatformCloud/` owns cloud business capabilities:
  `PackageRegistry/` is split into `ControlPlane/`, `PublicationBuilder/`,
  `StaticReadPlane/`, `MirrorSync/`, and `ReleaseManagement/`, while
  `DeveloperWorkspace/` contains job queue request semantics, worker runtime,
  compile-container/workspace factories, and the deterministic workspace compile
  stress harness. `DocumentationGovernance/` contains the documentation
  collection model, ownership rules, and docs change planner.
  `EcosystemManagement/` contains the Styio ecosystem repository manifest and
  release-train planner for heterogeneous runtimes.
- `PlatformService/` owns external service entrypoints: HTTP adapters, routing,
  route catalogs, `PlatformOps/` rate-limit/metrics helpers, and the platform
  daemon. `Router.cpp` dispatches only; capability route handlers live beside
  the capability they mutate.
`CMakeLists.txt` and this README stay at the `src/` root as source-tree
metadata instead of runtime capability code.

Pafio owns manifest, lock, resolution, metadata, and project workflow
contracts. Styio owns compiler-facing plans, diagnostics, receipts, and runtime
events. Platform consumes those contracts through worker processes instead of
duplicating their implementations.

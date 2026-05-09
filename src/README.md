# styio-platform Source

This source tree contains the first migrated platform kernel. It keeps imported
`spio::` namespaces stable while the repo split settles, but source ownership is
now grouped by platform capability.

- `PlatformCore/` owns core ability: manifest and lockfile support, dependency
  resolution, registry client access, and toolchain/source-build behavior.
- `PlatformCore/System/` owns the system layer: config loading,
  filesystem/process helpers, and centralized operating-system adapters.
- `PlatformStorage/` owns the storage layer:
  `PlatformCache/` contains cache and local state layout, and
  `PlatformPersistence/` contains persisted state records, the memory-backed
  development state store, Postgres migrations/state access, and object storage
  access for S3, filesystem, or memory-backed object stores.
- `PlatformSecurity/` owns security ability:
  `PlatformCA/` contains certificate authority, trust-anchor lifecycle, and
  managed mTLS certificate issuance, `PlatformClientAuth/` contains mTLS
  identity parsing and operation-level authorization policy, and
  `SecurityHardening/` contains registry read/write security hooks and
  hardening extension points.
- `PlatformCloud/` owns cloud business capabilities:
  `PackageRegistry/` is split into `ControlPlane/`, `PublicationBuilder/`,
  `StaticReadPlane/`, and `MirrorSync/`, while `DeveloperWorkspace/` contains
  job queue request semantics, worker runtime, compile-container/workspace
  factories, and the deterministic workspace compile stress harness.
- `PlatformService/` owns external service entrypoints: HTTP adapters, routing,
  and the platform daemon.
- `SpioPlatformProtocols/` owns all Spio-to-Platform interaction payloads and
  serializers, including compile-plan v1, project-graph payloads, cloud
  execution policy, and cloud build job requests.

`CMakeLists.txt` and this README stay at the `src/` root as source-tree
metadata instead of runtime capability code.

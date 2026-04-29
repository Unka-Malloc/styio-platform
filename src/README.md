# styio-platform Source

This source tree contains the first migrated platform kernel. It keeps imported
`spio::` namespaces and file names stable while the repo split settles.

- `SpioPlan/` renders compile-plan v1 payloads.
- `SpioCloud/` renders cloud execution policy and build job requests.
- `PlatformService/` owns the native platform-control-plane router, the
  Boost.Beast/Asio HTTP adapter with POSIX fallback, optional Postgres store,
  workgroup cluster registry, worker runtime, mirror sync runtime, and daemon
  entrypoint.
- `SpioCore/`, `SpioManifest/`, `SpioResolve/`, `SpioRegistryClient/`,
  `SpioSecurity/`, and `SpioToolchain/` are the package-manager client
  dependencies needed to validate the platform payloads.
- `spio_cloud_stress/` owns the deterministic cloud compile stress harness.

New service implementation should prefer platform-owned names and APIs; the
imported names here are compatibility scaffolding.

# styio-platform Tests

Tests cover the migrated platform kernel and service contract packages.

- `native/` validates compile-plan and cloud job request behavior through CTest.
- `interop/` validates machine-readable contract packages and the local HTTP
  smoke path for `styio-platformd --serve-once`.
- `unit/` validates Python service tooling such as the cloud stress harness.
- `scripts/k8s-smoke.py` provides the opt-in Podman/Buildah, Helm, and kind
  deployment acceptance path for the primary, worker, mirror, Postgres, PVC,
  and workgroup cluster-registration topology.

# Dependency Usage Boundary

**Purpose:** Record dependency authorization boundaries for `styio-platform`.

**Last updated:** 2026-04-29

`styio-platform` is an Apache-2.0 C++/Python source project. Its current build and test dependency boundary is:

- CMake `FetchContent` downloads `tomlplusplus` for TOML parsing, `nlohmann_json` for JSON serialization, and `googletest` for native test execution.
- CMake `Threads::Threads` is resolved through the built-in `FindThreads` system linkage target for the local HTTP listener. It does not vendor source code, require commercial authorization, or add a private package source.
- CI installs `libboost-dev` from the Ubuntu package repository so the preferred HTTP adapter can compile against Boost.Beast and Boost.Asio headers. The repository does not vendor Boost source or redistribute a modified Boost package. When those headers are unavailable, the daemon uses the source-local POSIX socket fallback.
- `PostgreSQL` in `src/CMakeLists.txt` is resolved through CMake `FindPostgreSQL` and links only the libpq client library when distro packages provide `libpq-dev`/`libpq5`. PostgreSQL/libpq is used under the PostgreSQL License boundary as an OS-packaged client driver for the optional Postgres state backend; the repository does not vendor PostgreSQL source, run a private package registry, or redistribute a modified libpq package. Local builds without libpq still compile the in-memory backend and emit a clear unavailable-driver error if `STYIO_PLATFORM_STATE_BACKEND=postgres` is selected.
- The runtime `Containerfile` builds `styio-platformd`, `spio`, and `styio` from source using OCI-compatible Podman or Buildah tooling and fails if the expected binaries are not produced. The source repositories and refs are explicit build arguments and default to the public `eBioRing` `ai-dev` branches.
- The Helm chart under `deploy/helm/styio-platform` is the Kubernetes deployment manifest source for primary, worker, mirror, Postgres, and PVC resources. It does not vendor a third-party chart.
- The kind smoke uses Podman, optional Buildah, kind, kubectl, and Helm as external validation tools. These tools are open CI/test infrastructure and are not linked into the platform daemon.
- The one-command development environment uses the same Podman or Buildah, kind, kubectl, and Helm process boundary through `scripts/styio-platform dev up`; it reads YAML defaults from `config/styio-platform-tools.yaml`, registers local workgroup membership through the platform HTTP API, and does not introduce an additional runtime framework.
- Repository tool configuration is YAML. `scripts/styio_yaml.py` implements the narrow YAML mapping subset needed by the local tools using only the Python standard library, so no PyYAML or YAML framework dependency is introduced. JSON remains available for API contracts, wire payloads, and generated audit/report data.
- The canonical open CI/CD artifact is the Tekton pipeline under `deploy/tekton/styio-platform-ci`. GitHub workflows are compatibility wrappers for repository-hosted status checks and must keep delegating to repo-local scripts rather than becoming the source of truth.
- Repository Python scripts and tests use the Python standard library only.
- The current service kernel implements the local HTTP listener, optional Postgres persistence, workgroup cluster registration, filesystem-backed Kubernetes storage, real worker process execution, and mirror PVC synchronization as explicit service-side dependencies.
- Runtime registry, control-plane, VM deployment, and transport checks may invoke system tools such as `curl`, `git`, `tar`, `cmake`, `openssl`, `systemctl`, and the configured `styio` compiler binary through explicit process boundaries.

Dependency policy:

- No dependency may require commercial authorization, paid licensing, subscription access, membership access, trial-only terms, proprietary-use approval, or private registry access.
- Any future dependency must be listed here with its license evidence, source boundary, and usage boundary before it can pass audit.
- Dependencies used only for tests must stay test-scoped and must not become runtime requirements without this file being updated.
- Generated reports and gate summaries must summarize dependency and license evidence without copying target repository source.

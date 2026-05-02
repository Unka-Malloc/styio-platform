# styio-platform

`styio-platform` is the globally scalable cloud-computing service platform for
the Styio ecosystem. It owns hosted compile services, multi-region deployment
nodes, package repository distribution, mirror synchronization, registry
control-plane server surfaces, and extensible cloud-service validation tooling
that previously lived inside `styio-spio`.

This repository is downstream of `styio` and `styio-spio`:

- `styio` remains the compiler and language implementation source of truth.
- `styio-spio` remains the package manager, resolver, pack, publish, and client
  workflow surface.
- `styio-platform` consumes those contracts to run hosted workspaces, compile
  jobs, registry server control planes, cross-network deployment nodes, mirror
  sites, and cloud stress gates.

The first migrated kernel intentionally keeps the imported C++ namespace and
contract names stable while the repo boundary settles. New platform services
should live here first. `styio-spio` should only retain local package-manager
client code, offline package workflows, local import/export paths, and
compatibility shims that users need without connecting to the platform.

The V1 cloud service plan is a native platform kernel, not a generated API
toolchain. Its executable contract source is the repo-native JSON package under
`contracts/platform-control-plane/v1/`, with examples and gates validating that
package directly.

## Product Role

- global hosted compile and execution platform
- default hosted compile target for Styio workspaces
- standard C++/LLVM development and compile environment for every workspace
- native C++ invocation path for mixed Styio/C++ workloads
- C++ fallback path when Styio cannot express or execute part of a workload
- package repository distribution foundation for `styio-spio`
- multi-region and cross-network deployment node control plane
- mirror-site synchronization and registry replication
- extensible cloud service APIs consumed by local and hosted clients

## V1 Service Kernel

The first runnable cloud service kernel targets a pure C++ implementation:

- HTTP service layer built on Boost.Beast and Boost.Asio
- Postgres for durable control-plane state, job lifecycle, and audit metadata
- provider-neutral object storage with S3 as the first deployment backend
- mTLS between operators, regional nodes, workers, and internal service callers
- single-region runnable deployment before multi-region routing is promoted

V1 must boot as a small regional node that can expose health, node
introspection, hosted job lifecycle, worker lifecycle, and mirror freshness
status from the native JSON platform-control-plane contract.

The first network adapter is available through `styio-platformd --serve`. It
binds the native `PlatformRouter` to a local HTTP listener, preferring
Boost.Beast/Asio when those headers are available and using a synchronous POSIX
fallback otherwise. Set `STYIO_PLATFORM_TLS_ENABLED=1` with
`STYIO_PLATFORM_MTLS_CA`, `STYIO_PLATFORM_MTLS_CERT`, and
`STYIO_PLATFORM_MTLS_KEY` to terminate TLS in-process and derive service
identity from the client certificate URI SAN. Proxied identity headers remain
available only when `STYIO_PLATFORM_TRUST_PROXY_IDENTITY_HEADERS=1`.

The first multi-node deployment target is Kubernetes through
`deploy/helm/styio-platform`. The chart deploys a primary control plane, a
real compile worker, a mirror node, Postgres, and filesystem-backed
registry/object storage with PVCs retained for staging, workspaces, and local
mirror caches. Production S3 publication remains opt-in through Helm
`objectStore` values.
`styio-platformd --migrate` applies the control-plane schema,
`styio-platformd --worker` claims Git-backed build jobs and invokes
`spio build`, and `styio-platformd --sync-mirror-once` copies the registry v2
filesystem layout from the primary PVC into the mirror PVC and records
freshness in Postgres.

## One-Command Development Environment

The developer environment entrypoint is:

```sh
./scripts/styio-platform dev up
```

This command reads tool defaults from `config/styio-platform-tools.yaml`, builds
the OCI image with Podman by default, creates or reuses a kind cluster through
kind's Podman provider, installs the Helm chart, waits for Postgres, primary,
worker, and mirror rollouts, starts local port-forwarding, and prints the
platform and mirror endpoints. Tool configuration in this repository is YAML;
the JSON files under `contracts/` remain API contract and example data.

Useful follow-up commands:

```sh
./scripts/styio-platform dev status
./scripts/styio-platform dev workgroup-status
./scripts/styio-platform dev shell
./scripts/styio-platform dev logs --component worker
./scripts/styio-platform dev down --delete-cluster
```

`dev up` registers the local cluster into the default workgroup from
`config/styio-platform-tools.yaml`. To merge two local platform environments,
start both with different cluster/namespace/release or port settings, then run:

```sh
./scripts/styio-platform dev join-workgroup \
  --peer http://127.0.0.1:8788/api/styio-platform/v1
```

The join command registers both primary endpoints into the same control-plane
workgroup and records the local Kubernetes service DNS endpoint for in-cluster
traffic. The built-in policy accepts registration only from the platform tenant
with `operator`, `control-plane`, or `cluster-registrar` identity roles, and
uses the configured registration token when one is set.

Override defaults either with CLI flags or by passing another YAML config:

```sh
./scripts/styio-platform dev up --config config/styio-platform-tools.yaml
```

The environment runtime is Linux OCI containers on Kubernetes. Native Linux is
the primary local host. macOS should use Podman machine or a remote Kubernetes
context, and Windows should use WSL2 or a remote Kubernetes context rather than
native Windows process execution.

## Validate

```sh
cmake -S . -B build-codex -DSTYIO_PLATFORM_BUILD_TESTS=ON
cmake --build build-codex
ctest --test-dir build-codex --output-on-failure
ctest --test-dir build-codex -R styio_platform_http_smoke --output-on-failure
python3 -m unittest tests/unit/test_cloud_compile_stress.py
python3 scripts/docs-audit.py
python3 scripts/repo-hygiene-gate.py --mode tracked
```

The Kubernetes smoke uses open OCI tooling. It requires Podman, kind, kubectl,
and Helm by default; `--image-builder buildah` switches image construction to
Buildah while still using kind's Podman provider for the local cluster:

```sh
python3 scripts/k8s-smoke.py
```

The Kubernetes-native CI source of truth is the Tekton pipeline under
`deploy/tekton/styio-platform-ci/`. GitHub workflows remain compatibility
wrappers for repository-hosted status checks; they call the same repository
scripts and use Podman/Buildah instead of Docker CLI.

For direct Helm installs, set `postgres.password` or provide
`postgres.externalDsn`; the repository default intentionally leaves the
password empty so credentials are not stored in tracked values. The default
object store is filesystem-backed for local smoke runs; setting
`objectStore.provider=s3` requires non-empty `objectStore.bucket` and
`objectStore.endpoint` values.

The common delivery wrapper keeps this smoke opt-in for local checkpoints:

```sh
./scripts/delivery-gate.sh --mode checkpoint --with-k8s
```

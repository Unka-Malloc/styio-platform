# Platform Regional Node Runbook

**Purpose:** Define the operating expectations for multi-region and cross-network `styio-platform` deployment nodes.

**Last updated:** 2026-04-29

## Node Roles

Regional nodes may provide:

- compile worker capacity
- hosted workspace control-plane routing
- registry read mirrors
- registry write forwarding
- mirror synchronization and health reporting

Node roles must be explicit in deployment metadata so clients and operators know
whether a node is authoritative, read-only, write-forwarding, or compile-only.

## V1 Runnable Kernel

The first regional node must run as a single-region C++ service before any
multi-region deployment is promoted. Its target stack is Boost.Beast and
Boost.Asio for HTTP/network execution, Postgres for durable control-plane
state, provider-neutral object storage with S3 as the first backend, and mTLS
for operator, node, worker, and internal service traffic.

The node must expose only the native JSON platform-control-plane V1 route
families until matching gates exist for a broader topology.

The Kubernetes V1 deployment is the first multi-node runnable topology. The
Helm chart under `deploy/helm/styio-platform` starts:

- a primary control-plane pod running `styio-platformd --serve`
- a Postgres StatefulSet used by `STYIO_PLATFORM_STATE_BACKEND=postgres`
- a worker deployment running `styio-platformd --worker`
- a mirror deployment with control, static registry read, and sync containers
- PVCs for registry, mirror registry, workspaces, and artifacts

Primary pods must run `styio-platformd --migrate` before serving traffic.
Workers claim jobs through the platform HTTP API, fetch `job_request.source.origin`
through the shared `PlatformCore/SourceFetch` Git fetcher, run
`pafio build --manifest-path <relative path>` with `PAFIO_STYIO_BIN` set to the
system-provided Styio executable, and write
stdout, stderr, and result metadata under the artifact PVC. Mirror sync copies
`config.json`, `trust/`, `artifacts/`, `index/`, and `log/` from the primary
registry PVC to the mirror registry PVC, then records the freshness cursor in
Postgres.

For local development, operators can start the same topology with one command:

```bash
./scripts/styio-platform dev up
```

The command targets Linux OCI containers on Kubernetes. Native Linux is the
primary local host; macOS should use Podman machine or a remote Kubernetes
context, and Windows should use WSL2 or a remote Kubernetes context. Local tool
defaults come from `config/styio-platform-tools.yaml`; JSON files under
`contracts/` remain protocol contracts rather than operator configuration.

Each dev environment self-registers into the configured workgroup after local
port-forwarding is ready. Operators can merge another local primary endpoint
with:

```bash
./scripts/styio-platform dev join-workgroup --peer http://127.0.0.1:8788/api/styio-platform/v1
```

The registration policy is enforced by the platform control plane: only the
configured platform tenant may register clusters, only `operator`,
`control-plane`, or `cluster-registrar` identities can write workgroup
membership, and `worker`, `mirror`, and `registry-writer` roles are read-only.
When `STYIO_PLATFORM_WORKGROUP_REGISTRATION_TOKEN` is set, the request must
also carry the same token. Registered records include both host port-forwarded
endpoints and Kubernetes service DNS endpoints so same-cluster dev nodes can
discover each other without leaving the local workgroup.

## Cross-Network Deployment

Cross-network deployments must avoid assuming a single low-latency control
plane. Contracts should expose region, mirror freshness, accepted write origin,
and retry/replay behavior instead of hiding topology behind one opaque endpoint.

## Required Gates

Before a node role is promoted, validate:

- contract compatibility for hosted and registry APIs
- native JSON platform-control-plane contract and example compatibility
- mirror freshness and replay behavior
- package read availability from the regional mirror
- worker-pool health for compile-capable nodes
- workgroup cluster registration and role-policy enforcement
- failure isolation between regions
- Podman or Buildah, Helm, and kind smoke through `python3 scripts/k8s-smoke.py`

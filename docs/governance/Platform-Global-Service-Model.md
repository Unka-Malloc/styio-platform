# Platform Global Service Model

**Purpose:** Define `styio-platform` as the global cloud-computing and package-distribution foundation that serves `styio-spio` clients and hosted products.

**Last updated:** 2026-04-29

## Mission

`styio-platform` provides the globally scalable service layer for Styio:
hosted compile execution, extensible cloud services, package repository
distribution, registry control planes, regional deployment nodes, and mirror
site synchronization.

## Service Boundary

The platform owns:

- hosted workspace and compile execution APIs
- default Styio compile target and standard C++/LLVM workspace environment
- native C++ invocation and mixed Styio/C++ compile contracts
- queue, scheduler, worker-pool, and regional node contracts
- registry server write/control planes
- package repository distribution and mirror replication
- cross-network deployment topology and service health contracts
- service-side observability, replay, and stress validation
- native JSON platform-control-plane contracts, examples, and gates

The platform does not own:

- local manifest editing or lockfile UX
- offline package import/export commands
- project-local compiler optimization policy
- `styio-spio` package-manager CLI grammar

## Availability Model

The service model must support independent regional nodes, cross-network
deployments, and mirror sites. A regional node may host compile workers,
registry read replicas, write-forwarding control planes, or all of them.

Package repository distribution must tolerate mirror lag explicitly. Clients
must be able to distinguish authoritative write acceptance from eventually
consistent mirror availability.

The V1 service target is a single-region runnable kernel. It must prove health,
node introspection, hosted job lifecycle, worker lifecycle, workgroup cluster
registration, and mirror freshness/replay behavior before multi-region routing
or provider fan-out is treated as production scope.

The first deployment proof is a Kubernetes primary/worker/mirror topology. It
keeps write acceptance centralized in the primary control plane, scales compile
capacity through worker replicas, and treats mirror freshness as a persisted
control-plane record rather than an implication of successful static reads.

## Implementation Target

The V1 cloud service implementation target is pure C++:

- Boost.Beast and Boost.Asio for HTTP and async network execution
- Postgres for control-plane state, hosted job lifecycle, mirror status, and
  workgroup cluster registration records
- PVC-backed filesystem storage for the first Kubernetes registry, workspace,
  and artifact deployment; provider-neutral object storage remains the follow-up
  production backend
- OCI images built with Podman/Buildah, a Helm chart, and a Tekton pipeline for
  open Kubernetes deployment validation
- mTLS across operator, regional-node, worker, and internal service traffic

The service contract is maintained as native JSON under
`contracts/platform-control-plane/v1/`; generated third-party API descriptions
are not part of the platform governance source of truth.

## Client Relationship

`styio-spio` is a local-first client. Platform access improves discovery,
remote package distribution, hosted compile, and synchronization, but a project
with available offline packages must still work through local package-manager
paths without connecting to `styio-platform`.

## Workspace Compile Model

Hosted workspaces default to Styio compilation. The same workspace is also a
standard C++/LLVM development and compile environment. Native C++ invocation is
not an escape hatch of last resort; it is a first-class path for mixed
workloads.

If Styio cannot express, optimize, or execute a workload segment, the platform
must be able to degrade that segment into C++/LLVM while preserving build
ordering, package inputs, diagnostics, artifacts, and runtime handoff. Smooth
mixed Styio/C++ compilation is the first design goal of the workspace model.

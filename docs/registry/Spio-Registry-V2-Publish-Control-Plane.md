# Spio Registry V2 Publish Control Plane

**Purpose:** Define the server-side responsibilities that produce the immutable `spio` registry `v2` static read plane without leaking dynamic publish behavior into the client-facing distribution protocol.

**Last updated:** 2026-05-02

## 1. Role

The publish control plane is the only trusted writer for a production `v2` registry.

It is responsible for:

- publisher authentication
- namespace authorization
- artifact upload admission
- digest verification
- package release admission
- signed metadata generation
- transparency-log append
- yank and deprecate state transitions

It is not part of the static read-plane protocol. Clients that install packages do not need this API surface.

The current versioned HTTP service contract for this plane lives in:

- [`../../contracts/registry-control-plane/v1/registry-control-plane.contract.json`](../../contracts/registry-control-plane/v1/registry-control-plane.contract.json)
- [`../../contracts/registry-control-plane/v1/registry-control-plane.examples.json`](../../contracts/registry-control-plane/v1/registry-control-plane.examples.json)
- [`./Spio-Registry-Control-Plane-Contract.md`](./Spio-Registry-Control-Plane-Contract.md)

## 2. Required Write-Side Guarantees

The publish service must guarantee:

- immutable artifact objects
- append-only index evolution
- append-only transparency-log evolution
- atomic publication of new metadata snapshots
- no silent overwrite of an existing release body

Public read origins must never accept anonymous direct `PUT` for package publication in the `v2` model.

## 3. Publish Flow

Recommended logical flow:

1. authenticate publisher
2. verify namespace ownership
3. upload and hash source artifact
4. upload and hash optional binary artifacts
5. verify package manifest and dependency policy
6. append release record to the package index
7. update namespace targets metadata
8. update snapshot and timestamp metadata
9. append a transparency-log leaf
10. sign the checkpoint and expose the new static root state

## 4. Release-State Semantics

Required release-state actions:

- `publish`
- `yank`
- `deprecate`
- `undeprecate` by appending a new state record

Rules:

- original artifacts remain immutable
- release records are never silently rewritten
- repeated publish of the same version with a different payload must be rejected

## 5. Namespace Governance

Industrial operation requires explicit namespace governance:

- owner list
- delegation rules
- key rotation policy
- audit trail for state changes

The static read plane only carries the signed outputs of that governance model.

## 6. Security Baseline

A production publish service should provide at least:

- authenticated publishers
- namespace-scoped authorization
- staged uploads
- digest-verified commit
- signing-key isolation from general request handlers
- audit logs for publish/yank/deprecate operations

## 7. Executable Surfaces

The tracked executable surfaces are:

- `v2` static contract pack
- local role-key generation
- local publish worker for direct source-package commits into a `v2` root
- static-root verification gates
- native C++ publish-control-plane routing in `styio-platformd`
- S3-backed static registry object publication for `config/`, `trust/`,
  `index/`, `artifacts/`, and `log/`
- in-process TLS/mTLS termination for the hosted control-plane daemon

The local worker entrypoint is:

- `scripts/registry-v2-publish.py`

The hosted C++ entrypoint is:

- `styio-platformd --serve`

It updates:

- source-artifact storage
- append-only package indexes
- namespace targets metadata
- snapshot and timestamp metadata
- transparency-log checkpoint and leaves

When `STYIO_PLATFORM_OBJECT_STORE_PROVIDER=s3`, the hosted C++ route uses the
local registry root as a staging cache, synchronizes existing remote metadata
from S3, writes signed registry v2 metadata locally, and uploads the resulting
static read-plane objects back to S3. Deployments must set bucket, endpoint,
region, credentials, and a public `STYIO_PLATFORM_REGISTRY_READ_ROOT_URL` when
clients consume the read plane through HTTP or a CDN.

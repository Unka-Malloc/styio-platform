# Platform Security Boundary

**Purpose:** Define the initial trust split for hosted compile and registry control-plane services.

**Last updated:** 2026-05-02

## Boundary

`styio-platform` treats local `styio-spio` manifests and lockfiles as client
inputs. It validates execution lanes, risk classes, source revisions, and
registry write requests before dispatching work to hosted workers or server
control planes.

Compiler-private execution remains behind `styio`; package-manager credential
storage remains in `styio-spio` until a platform credential service is designed.

## V1 Service Trust Rules

The first cloud service kernel requires mTLS for operator access,
regional-node links, worker-internal lifecycle calls, and internal service
callers. Public client auth can evolve separately, but service-to-service
traffic may not rely on bearer-only trust inside the platform boundary.
`styio-platformd` can terminate TLS/mTLS directly and derives request identity
from the client certificate URI SAN. Header-carried identity is accepted only
when deployments explicitly trust an upstream proxy through
`STYIO_PLATFORM_TRUST_PROXY_IDENTITY_HEADERS`.

Workgroup cluster registration is a control-plane write. The default policy
accepts writes only from the configured platform tenant and from `operator`,
`control-plane`, or `cluster-registrar` identities; `worker`, `mirror`, and
`registry-writer` identities may read registered clusters but cannot mutate
membership. Deployments can require `STYIO_PLATFORM_WORKGROUP_REGISTRATION_TOKEN`
as a second local registration factor.

Postgres is the durable control-plane state boundary. Provider-neutral object
storage, with S3 first, stores artifacts and replayable registry objects;
database rows must keep object references, digests, lifecycle state, and audit
metadata. Registry v2 publication writes signed `config/`, `trust/`, `index/`,
`artifacts/`, and `log/` objects to the configured S3 read plane while using
the local filesystem only as a staging cache.

Registry descriptor publication is platform-owned. The control plane exposes
`GET /api/spio-registry-control/v1/descriptor` to authenticated internal roles
and returns the public read root plus the pinned `trust/root.json` SHA-256.
`styio-spio` imports that descriptor into local client state before consuming
remote HTTP registries, so the public package manager does not establish trust
from registry-hosted metadata alone.

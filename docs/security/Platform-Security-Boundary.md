# Platform Security Boundary

**Purpose:** Define the initial trust split for hosted compile and registry control-plane services.

**Last updated:** 2026-04-29

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

Workgroup cluster registration is a control-plane write. The default policy
accepts writes only from the configured platform tenant and from `operator`,
`control-plane`, or `cluster-registrar` identities; `worker`, `mirror`, and
`registry-writer` identities may read registered clusters but cannot mutate
membership. Deployments can require `STYIO_PLATFORM_WORKGROUP_REGISTRATION_TOKEN`
as a second local registration factor.

Postgres is the durable control-plane state boundary. Provider-neutral object
storage, with S3 first, stores artifacts and replayable objects; database rows
must keep object references, digests, lifecycle state, and audit metadata.

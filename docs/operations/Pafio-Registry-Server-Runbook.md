# Pafio Registry Origin Runbook

**Purpose:** Provide the executable validation and deployment procedure for a shared `pafio` registry `v2` origin without mixing it with client cache behavior or hosted publish-service policy.

**Last updated:** 2026-05-02

## 1. Scope

This runbook owns:

- server-side smoke validation commands
- publish/read origin validation flow
- deployment checklist for the current registry `v2` static root and publish-control-plane boundary
- hosted registry and mirror service validation in `styio-platform`

Server policy lives in [../registry/Pafio-Registry-Control-Plane-Contract.md](../registry/Pafio-Registry-Control-Plane-Contract.md) and [../registry/Pafio-Registry-V2-Publish-Control-Plane.md](../registry/Pafio-Registry-V2-Publish-Control-Plane.md). Deployment baseline still lives in [../registry/Pafio-Registry-Deployment-Baseline.md](../registry/Pafio-Registry-Deployment-Baseline.md).

`pafio-nightly` remains the offline-capable local package manager. This runbook
must not require client cache, vendored, or imported packages to contact a
platform mirror before they can be used locally.

## 2. Preconditions

- native `pafio` binary is buildable locally
- target registry root already exposes the canonical shared layout
- if publish and fetch roots are split, replication or synchronization is already configured between them
- Linux VM deployments use Python 3, OpenSSL, and systemd
- manifest-based publish requests on the server need a local `pafio` binary path; archive-based publish requests can run without one

Recommended local build entry:

```text
./scripts/native-check.sh
```

## 3. VM One-Command Deployment Bundle

Build the VM bundle from a clean `styio-platform` checkout:

```text
./scripts/package-registry-server.sh
```

Copy the generated tarball to the VM and run the installer:

```text
tar -xzf styio-platform-registry-server-<version>.tar.gz
cd styio-platform-registry-server-<version>
sudo ./install.sh
```

The default install creates:

- `/opt/styio-platform-registry` runtime scripts and Python modules
- `/var/lib/styio-platform/registry/v2` initialized registry static root
- `/var/lib/styio-platform/registry/v2-keys` server-side registry role keys
- `/etc/styio-platform/styio-registry.env` deployment environment file
- `styio-registry-control.service` for `/api/pafio-registry-control/v1/status|publish|verify`
- `styio-registry-read.service` for read-only static package distribution

Safe default binds:

- control plane: `127.0.0.1:8787`
- read plane: `0.0.0.0:8788`

Use these flags when the VM has a non-default `pafio` binary or a different
network policy:

```text
sudo ./install.sh --pafio-bin /opt/pafio/bin/pafio --control-bind 127.0.0.1 --read-bind 0.0.0.0
```

The installer initializes an empty but valid registry root before starting the
services and runs:

```text
/opt/styio-platform-registry/scripts/registry-v2-vm-smoke.py \
  --control-url http://127.0.0.1:8787 \
  --read-url http://127.0.0.1:8788 \
  --json
```

Post-install operator checks:

```text
systemctl status styio-registry-control.service
systemctl status styio-registry-read.service
curl -fsS http://127.0.0.1:8787/api/pafio-registry-control/v1/status
curl -fsS http://127.0.0.1:8788/config.json
```

The control plane stays bound to localhost by default because publish is a write
operation. Put it behind a VM-local gateway, tunnel, or private network policy
before exposing it outside the host.

## 4. Single-Origin Validation

Use this when one origin handles both publish and fetch:

```text
./scripts/registry-server-gate.py --registry-root https://registry.example.internal --pafio-bin ./build-codex/bin/pafio --json
```

What it proves:

- publish commits a valid registry `v2` release
- duplicate publish is rejected
- the new package is immediately fetchable from the same static root
- verify confirms the static root after the publish path changes it

If the write origin sits behind an upload gateway that expects fixed headers, pass them explicitly:

```text
./scripts/registry-server-gate.py --registry-root https://registry-upload.example.internal --publish-header 'X-Pafio-Write-Token: dev-token' --pafio-bin ./build-codex/bin/pafio --json
```

If the deployment links a private security module and the write-origin rules should live in a reusable file instead of command-line headers:

```text
./scripts/registry-server-gate.py --registry-root https://registry-upload.example.internal --publish-policy-file /etc/pafio/publish-policy.toml --pafio-bin ./build-codex/bin/pafio --json
```

## 5. Split Publish and Fetch Origins

Use this when write traffic goes to an upload origin and read traffic goes to a download origin or CDN:

```text
./scripts/registry-server-gate.py --publish-root https://registry-upload.example.internal --fetch-root https://registry.example.internal --sync-timeout-seconds 30 --pafio-bin ./build-codex/bin/pafio --json
```

Notes:

- `--sync-timeout-seconds` is the publish-to-fetch visibility budget
- keep it at `0` when the read root should be immediately consistent
- raise it only when the read root is populated by asynchronous replication or CDN propagation

## 6. Promotion Workflow for Split Origins

Use this when the write root and read root are backed by different local serving roots or mounted storage views:

```text
./scripts/registry-promote.py --source-root /srv/pafio/upload-root --dest-root /srv/pafio/read-root --json
```

Scoped promotion is also supported:

```text
./scripts/registry-promote.py --source-root /srv/pafio/upload-root --dest-root /srv/pafio/read-root --package acme/util --version 0.2.0 --json
```

What it proves:

- the source root has a valid registry `v2` shape
- destination objects remain immutable
- `config/`, `trust/`, `index/`, `artifacts/`, and `log/` objects are copied consistently
- repeated promotion is idempotent

## 7. Local Split-Origin Smoke Test

The repository black-box gate for the recommended upload/download split is:

```text
bash ./tests/interop/registry-split-origin-promotion.sh ./build-codex/bin/pafio
```

This validates `publish -> promote -> fetch`.

## 8. Remote-Shape Split-Origin Smoke Test

The repository also ships a closer deployment-shape smoke test:

```text
bash ./tests/interop/registry-split-origin-http.sh ./build-codex/bin/pafio
```

This validates:

- HTTP publish to a write origin
- promotion from the write backing root into the read backing root
- HTTP fetch from a separate read origin
- mirror-style lag and replay assumptions before a read endpoint is advertised

Use this when you want to rehearse the recommended "internal upload origin plus read-only download origin" topology end-to-end.

## 9. Local HTTP Smoke Test

The repository black-box gate uses the local immutable test server:

```text
bash ./tests/interop/registry-server-gate.sh ./build-codex/bin/pafio
```

Use this before touching a real shared registry.

Auth-bearing write-origin smoke tests are intentionally not shipped in the tracked public tree. If a private security module is linked, keep those validations under `tests-private/` and `docs-private/` instead of restoring them to `tests/interop/`.

## 10. Production Checklist

- use `https` for remote publish and fetch roots
- terminate service-to-service TLS/mTLS in `styio-platformd` with
  `STYIO_PLATFORM_TLS_ENABLED=1`, `STYIO_PLATFORM_MTLS_CA`,
  `STYIO_PLATFORM_MTLS_CERT`, and `STYIO_PLATFORM_MTLS_KEY`
- set `STYIO_PLATFORM_TRUST_PROXY_IDENTITY_HEADERS=0` when traffic is not
  behind a trusted identity-terminating proxy
- set `STYIO_PLATFORM_OBJECT_STORE_PROVIDER=s3` with bucket, endpoint, region,
  prefix, access key, and secret key values for the registry read plane
- set `STYIO_PLATFORM_REGISTRY_READ_ROOT_URL` to the public HTTP(S) or CDN root
  that serves the S3-backed `config.json`, `trust/`, `index/`, `artifacts/`,
  and `log/` objects
- keep the VM installer smoke gate green before advertising the read endpoint
- keep write/control-plane exposure private unless a deployment-owned gateway enforces authorization
- preserve immutable object semantics for artifacts and log leaves
- reject overwrite attempts with `409 Conflict`
- retain audit logs for publication attempts
- keep `config/`, `trust/`, `index/`, `artifacts/`, and `log/` backed up together
- keep upload and download origins synchronized before enabling client installs
- keep the promotion step auditable and repeatable when upload and read origins are separate
- if the write origin requires gateway headers, keep them scoped to the upload path only and do not require them from public read origins
- if a policy file is used for write-origin headers, keep it outside the source tree and rotate its contents through deployment config rather than project manifests
- if a named profile is used, provision it through the private security module under deployment-owned state rather than from project state

## 11. Helm S3 and mTLS Baseline

The Kubernetes chart defaults to filesystem-backed PVC storage for local smoke
runs. Production registry deployments must set the S3 registry values under
`objectStore`, `registry`, and `mtls` values:

```text
helm upgrade --install styio deploy/helm/styio-platform \
  --set objectStore.provider=s3 \
  --set objectStore.bucket=styio-registry \
  --set objectStore.endpoint=https://s3.example.internal \
  --set objectStore.region=us-east-1 \
  --set objectStore.existingSecret=styio-registry-s3 \
  --set registry.readRootUrl=https://registry.example.internal \
  --set mtls.tlsEnabled=true \
  --set mtls.caSecretName=styio-platform-ca \
  --set mtls.serverSecretName=styio-platform-server \
  --set mtls.clientSecretName=styio-platform-worker
```

The object-store secret must provide `access-key-id`, `secret-access-key`, and
optionally `session-token`. The server secret is mounted into the control-plane
pods. The client secret is mounted into workers so HTTPS control-plane calls can
present a SPIFFE URI SAN accepted by the platform router. Helm rendering fails
when `objectStore.provider=s3` is selected without both `objectStore.bucket`
and `objectStore.endpoint`.

## 12. Failure Triage

- VM smoke fails on `config.json`:
  check `styio-registry-read.service`, read bind/port policy, and registry root permissions
- VM smoke fails on `verify`:
  check `styio-registry-control.service`, key-dir permissions, and partial root initialization
- publish fails immediately:
  check `PUT` support, write permissions, proxy limits, and immutable-path handling
- publish fails only at a gated write origin:
  check required header policy, `--registry-profile`, `--registry-policy-file`, and `--registry-header` values first
- duplicate publish succeeds:
  immutable object enforcement is broken
- publish succeeds but fetch fails:
  upload and download roots are not serving the same objects yet, or sync lag exceeds `--sync-timeout-seconds`
- promotion fails before any fetch:
  check source `v2` root validity, destination immutability conflicts, and backing-store path permissions
- fetch succeeds but later client installs fail integrity checks:
  inspect artifact corruption, proxy rewriting, and backing-store immutability

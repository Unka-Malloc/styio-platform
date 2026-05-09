# Platform Cloud Control-Plane Contract

**Purpose:** Define the machine contract for platform cloud execution policy, worker-pool routing keys, and project-local execution preferences.

**Last updated:** 2026-05-09

## Scope

This document owns:

- cloud execution policy terminology used by the native CLI
- the project-local cloud preference fields recorded in `spio-toolchain.lock`
- the JSON shape exposed through `spio cloud status --json`
- the JSON shape exposed through `spio cloud plan --json`
- the machine-readable cloud policy payload embedded in workflow success JSON
- the worker-pool key dimensions that remote execution must preserve

This document does not own:

- registry repository layout
- publish/fetch path contracts
- external `styio` binary compatibility
- remote API routes outside the native JSON control-plane packages

Those remain in the existing governance, registry, and `styio` contract documents.

## Route Ownership

The route-level HTTP contract for hosted workspace and deployment operations is owned by native JSON packages:

- [`./Platform-Hosted-Control-Plane-Contract.md`](./Platform-Hosted-Control-Plane-Contract.md)
- [`../../contracts/hosted-control-plane/v1/hosted-control-plane.contract.json`](../../contracts/hosted-control-plane/v1/hosted-control-plane.contract.json)
- [`../../contracts/hosted-control-plane/v1/hosted-control-plane.examples.json`](../../contracts/hosted-control-plane/v1/hosted-control-plane.examples.json)
- [`../../contracts/platform-control-plane/v1/platform-control-plane.contract.json`](../../contracts/platform-control-plane/v1/platform-control-plane.contract.json)
- [`../../contracts/platform-control-plane/v1/platform-control-plane.examples.json`](../../contracts/platform-control-plane/v1/platform-control-plane.examples.json)

This cloud-policy document still owns the execution-lane, risk-class, security-profile, cache-policy, and worker-pool semantics that those route payloads must preserve.

## V1 Cloud Service Target

The first remote service implementation must be a single-region runnable kernel
before any multi-region topology is promoted. The target stack is pure C++ with
Boost.Beast and Boost.Asio for HTTP and async networking, Postgres for durable
control-plane state, provider-neutral object storage with S3 as the first
backend, and mTLS for service-to-service and operator traffic.

The native JSON platform-control-plane package owns the executable route shape
for health, node introspection, hosted job lifecycle, worker lifecycle, and
mirror freshness/replay status. This document owns the cloud execution policy
semantics those routes expose.

## Implementation Rules

- `CloudBuildJobRequest` is created only through the domain factory that validates workflow invariants.
- `spio cloud plan --json` and workflow success payloads must serialize cloud policy and build-job payloads through shared contract serializers, not ad-hoc CLI JSON builders.
- compile-cloud stress validation must use the public framework described in [Platform Cloud Compile Stress Framework](./Platform-Cloud-Compile-Stress-Framework.md), not a page-local or service-local ad hoc benchmark.

## Terms

### Execution Lane

`ExecutionLane` accepts:

- `isolated`
- `warm-shared`

Normative rules:

- `isolated` is the default lane
- `warm-shared` is opt-in and may be downgraded by policy resolution
- callers must treat the resolved lane, not the preferred lane, as authoritative

### Risk Class

`RiskClass` accepts:

- `trusted-internal`
- `partner-controlled`
- `untrusted-user`

Normative rules:

- `untrusted-user` is the default
- `untrusted-user` must resolve to `isolated`
- `partner-controlled` may request `warm-shared`, but policy may downgrade it to `isolated`
- only `trusted-internal` may keep `warm-shared` by default

### Security Profile

`SecurityProfile` accepts:

- `sandbox-default`
- `partner-restricted`
- `trusted-warm`

Normative rules:

- `sandbox-default` is the default profile
- the resolved security profile follows the resolved lane and risk class
- callers may not assume the project-local preferred profile remains unchanged after policy resolution

## Project-Local State

`spio-toolchain.lock` remains the project-local source of truth for execution preferences.

Required persisted fields:

- `[toolchain]`
  - `mode = "binary" | "build"`
  - `channel = "stable" | "nightly"`
  - `build = "minimal"`
- `[cloud]`
  - `risk = "trusted-internal" | "partner-controlled" | "untrusted-user"`
  - `lane = "isolated" | "warm-shared"`
  - `security = "sandbox-default" | "partner-restricted" | "trusted-warm"`

Defaults:

- `mode = "binary"`
- `channel = "stable"`
- `build = "minimal"`
- `risk = "untrusted-user"`
- `lane = "isolated"`
- `security = "sandbox-default"`

## Machine-Readable Introspection

### `spio machine-info --json`

The self-description endpoint must advertise:

- `supported_contracts.cloud_execution_policy = [1]`
- `supported_contracts.worker_pool_keys = [1]`
- `supported_contracts.build_job_request = [1]`

This only indicates contract support. It does not imply that a remote scheduler or distributed worker system is active.

### `spio cloud status --json`

Canonical form:

```text
spio cloud status --json [--manifest-path <path>]
```

This command must report at least:

- selected manifest path
- project-local `toolchain_mode`
- project-local `channel`
- project-local `build_mode`
- persisted `risk_class`
- persisted `preferred_execution_lane`
- persisted `security_profile`
- `supported_execution_lanes`
- `supported_risk_classes`
- `supported_security_profiles`
- resolved `cloud` policy object

### `spio cloud plan --json`

Canonical form:

```text
spio cloud plan --json <build|run|test> ...
```

This command must report at least:

- `command = "cloud plan"`
- a top-level `job_request`
- `job_request.schema_version = 1`
- `job_request.api_path = "/api/styio-platform/v1/jobs"`
- `job_request.action`
- `job_request.toolchain`
- `job_request.workflow`
- `job_request.target`
- `job_request.source`
- resolved `job_request.cloud`

## Resolved Cloud Policy

The resolved `cloud` object must include:

- `execution_lane`
- `risk_class`
- `security_profile`
- `worker_trust_tier`
- `cache_policy`
- `worker_pool_key`

### Cache Policy

`cache_policy` reports:

- `shared_toolchain_read_only`
- `shared_source_read_only`
- `shared_registry_read_only`
- `worker_local_reuse`
- `shared_cache_promotion_eligible`
- `promotion_policy`

Normative baseline:

- shared toolchain, source, and registry cache mounts are always read-only
- worker-local reuse is allowed only for `warm-shared`
- shared-cache promotion eligibility is true only for `trusted-internal`

### Worker Pool Key

The worker-pool key dimensions are:

- `platform`
- `architecture`
- `toolchain_mode`
- `channel`
- `build_mode`
- `compiler_fingerprint`
- `base_image_revision`

These dimensions are stable contract keys. Remote worker pools may add keys, but must not redefine them.

## Command Grammar Ownership

The cloud preference surface uses the same public grammar family as the rest of the project-local toolchain state:

- `spio use <binary|build>`
- `spio set channel as <stable|nightly>`
- `spio set build as minimal`
- `spio set risk as <trusted-internal|partner-controlled|untrusted-user>`
- `spio set lane as <isolated|warm-shared>`
- `spio set security as <sandbox-default|partner-restricted|trusted-warm>`

The parser may accept compact compatibility forms without `as`, but normative docs and help output must keep the `as` spelling.

## Extension Rules

Remote execution extensions must preserve this document's:

- execution-lane vocabulary
- risk-class vocabulary
- security-profile vocabulary
- cache-policy semantics
- worker-pool key dimensions
- project-local state meanings

Remote APIs may extend these concepts, but they must not silently redefine them.

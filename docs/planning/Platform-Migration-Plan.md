# Platform Migration Plan

**Purpose:** Capture the migration boundary from `styio-spio` into the global `styio-platform` cloud-service and package-distribution foundation.

**Last updated:** 2026-04-24

## Current Boundary

`styio-platform` now owns the migrated compile-plan kernel, cloud job request
contracts, hosted control-plane API package, registry control-plane API package,
registry v2 server-side contract package, cloud compile stress tooling, package
repository distribution foundation, multi-region node model, mirror-site
synchronization model, Styio-by-default workspace compile model, standard
C++/LLVM workspace environment, and docs governance gates.

`styio-spio` remains the local-first package-manager client. It must keep
offline package workflows working when packages are already available locally,
and it retains compatibility shims until the CLI can consume platform APIs
instead of local service code.

## Next Cleanup

1. Replace `styio-spio` server scripts with calls into a released
   `styio-platform` tool package.
2. Move cloud and hosted-control-plane CI from `styio-spio` to
   `styio-platform` once this repository has remote `ai-dev` CI.
3. Keep `styio-spio` docs as upstream client docs and move service runbooks here.
4. Add regional node provisioning, cross-network replication, and mirror sync
   gates before removing server-side compatibility scripts from `styio-spio`.
5. Add executable gates for native C++ invocation and mixed Styio/C++ fallback
   before claiming hosted workspace compile closure.

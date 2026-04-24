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

## Validate

```sh
cmake -S . -B build-codex -DSTYIO_PLATFORM_BUILD_TESTS=ON
cmake --build build-codex
ctest --test-dir build-codex --output-on-failure
python3 -m unittest tests/unit/test_cloud_compile_stress.py
python3 scripts/docs-audit.py
python3 scripts/repo-hygiene-gate.py --mode tracked
```

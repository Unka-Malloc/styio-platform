# Contributing

`styio-platform` is an Apache-2.0 developer-preview project. Contributions
should preserve the existing module boundaries and contracts unless a change is
explicitly scoped as an architecture migration.

## Development Setup

Use the repository-local validation flow:

```sh
cmake -S . -B build-codex -DSTYIO_PLATFORM_BUILD_TESTS=ON
cmake --build build-codex -j 4
ctest --test-dir build-codex --output-on-failure
bash scripts/docs-gate.sh
python3 scripts/repo-hygiene-gate.py --mode working
git diff --check
```

For Kubernetes smoke testing, use:

```sh
python3 scripts/k8s-smoke.py
```

## Pull Requests

- Keep changes scoped to one behavior or migration step.
- Update contracts and examples when public API behavior changes.
- Update docs or manifests when module ownership changes.
- Add focused tests for new behavior and regression-sensitive fixes.
- Do not commit generated build outputs, local credentials, private keys, or
  machine-specific paths.

## Architecture Boundaries

- `PlatformCore` owns core platform primitives and client-side language-facing
  utilities.
- `PlatformStorage` owns persistence and cache adapters.
- `PlatformSecurity` owns certificate, identity, authorization, and hardening
  code.
- `PlatformCloud/PackageRegistry` owns registry business capability code.
- `PlatformCloud/DeveloperWorkspace` owns hosted workspace and compile worker
  behavior.
- `PlatformService` adapts these modules to HTTP and process entrypoints.

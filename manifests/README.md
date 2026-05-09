# Business Manifests

These manifests describe product capability boundaries for the modular monolith.
They are not source-file build lists. CMake remains the build graph source of
truth; manifests describe which platform modules, APIs, storage areas, security
scopes, and runtime adapters belong to each business capability.

The split is intentionally capability-first:

- `platform-foundation.yaml` is the non-removable base.
- `package-registry.yaml` owns the cloud package registry business.
- `developer-workspace.yaml` owns cloud developer workspaces and compile
  containers.
- `documentation-governance.yaml` owns documentation collections, source-of-
  truth rules, owner runbooks, and docs gate planning.
- `ecosystem-management.yaml` owns unified governance for the Styio ecosystem
  repositories without forcing one runtime.

When a business becomes large enough to extract into a service, its manifest is
the extraction checklist: CMake targets, protocol ownership, routes, storage,
security scopes, operational scripts, and runtime dependencies must be declared
here.

# Platform Workspace Compile Model

**Purpose:** Define the first-priority workspace design goal: Styio-by-default compilation with native C++/LLVM fallback and smooth mixed compile execution.

**Last updated:** 2026-05-09

## Mission

Every `styio-platform` workspace defaults to compiling Styio code. The same
workspace must also be a standard C++/LLVM development and compile environment.
Native C++ invocation is a first-class capability because some workloads will
need C++ even when Styio is the primary language path.

## Default Target

The default compile target is Styio:

- Styio source and compile-plan semantics remain the primary product path.
- Hosted workspace APIs should assume Styio first when no explicit target is
  selected.
- Package and toolchain metadata should preserve Styio intent even when a
  workload partially lowers into C++.

## C++ / LLVM Environment

Every platform workspace must expose a normal C++/LLVM toolchain surface:

- C++ source compilation
- LLVM diagnostics and artifact paths
- CMake-compatible project execution where applicable
- native C++ library or binary invocation from mixed workflows
- ABI and runtime metadata sufficient for hosted execution handoff

This environment is not separate from the Styio workspace. It is the lower-level
execution substrate that lets Styio and C++ interoperate.

## Mixed Compile Rule

When Styio cannot express, optimize, or execute a workload segment, the platform
must support degrading that segment to C++/LLVM without breaking the overall
workspace run:

- dependency ordering must remain deterministic
- diagnostics must identify whether they came from Styio or C++/LLVM
- artifacts must remain addressable through one workspace result envelope
- runtime handoff between Styio-generated and C++-native outputs must be
  explicit
- package inputs must remain traceable back to the package-manager graph

The success condition is not merely that C++ can be compiled somewhere. The
success condition is that mixed Styio/C++ compilation and execution remain
smooth inside one workspace.

## User-Bound Compile Containers

Hosted compile containers are logical worker-side build environments bound to a
single `tenant_id` and `user_id`. A bound container may hot-switch its current
workspace when the next job for the same user targets a different
`workspace_id`, but it must not accept jobs for another user.

The control plane records the container's `current_workspace_id` and
`workspace_generation`. Claiming a job with `compile_container_id` atomically
filters queued work to the container's tenant/user binding and increments the
workspace generation when a workspace switch is required. This preserves a warm
compiler/toolchain process while keeping user isolation explicit.

## Ownership

`styio-platform` owns the hosted workspace and cloud execution side of this
model. `styio` owns language and compiler semantics. `styio-spio` owns local
package-manager inputs and offline package availability that feed the workspace.

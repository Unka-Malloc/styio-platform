# Governance

**Purpose:** Publish normative platform contracts, ownership rules, workspace compile rules, and service-boundary policy.

**Last updated:** 2026-04-24

## Scope

Governance docs own the platform-service rules that downstream clients use to
interoperate with hosted workspaces, compile jobs, registry control planes, and
cloud-service extensions. They also own the workspace compile model: Styio is
the default compile target, and every workspace must remain a standard C++/LLVM
environment that supports native C++ invocation and mixed Styio/C++ fallback.

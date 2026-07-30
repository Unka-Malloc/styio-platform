# Styio Platform Owner Convergence Requirements

**Purpose:** Freeze the Platform-owned server and hosted-execution boundary for
the clean-break Pafio ecosystem.

**Last updated:** 2026-07-30

- **REQ-PLATFORM-CONV-001:** Platform is the sole owner of registry control
  plane, static registry distribution, hosted workspaces, cloud jobs, and
  workers.
- **REQ-PLATFORM-CONV-002:** Registry identity is
  `pafio-static-registry` and `/api/pafio-registry-control/v1`; published
  project manifests use `[pafio]` and `pafio.toml`.
- **REQ-PLATFORM-CONV-003:** Hosted workers invoke `pafio build`; Pafio remains
  the package/project workflow client and Styio remains the system compiler.
- **REQ-PLATFORM-CONV-004:** Platform contains no retired product identity,
  client business-logic copy, import facade, alias, or protocol fallback.
- **REQ-PLATFORM-CONV-005:** Focused Platform validation precedes one full
  repository regression and one fixed-revision ecosystem matrix.

Pafio resolver/workflow implementation, Styio compiler implementation, compiler
distribution, and Vityo adapter composition are owned by their respective
repositories and are out of scope here.

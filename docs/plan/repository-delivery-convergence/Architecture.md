# Styio Platform Owner Convergence Architecture

**Purpose:** Define the Platform side of the Pafio ecosystem handoff.

**Last updated:** 2026-07-30

`PackageRegistry` owns registry mutation, immutable publication generation,
static reads, trust metadata, distribution pointers, and mirror synchronization.
`DeveloperWorkspace` owns hosted workspaces, job queues, workers, and service
routing. The worker executes `pafio build` and supplies the externally installed
Styio path through the Pafio compiler-discovery contract.

Pafio owns manifest, lock, resolution, package lifecycle, and local project
workflows. Styio owns compilation, diagnostics, receipts, and runtime events.
Platform consumes those public contracts and does not embed either client's
business logic.

The migration is atomic across namespaces, build targets, scripts, schemas,
protocol values, environment variables, fixtures, and documentation. Python
registry code is imported directly from its owning layer; no compatibility
facade remains.

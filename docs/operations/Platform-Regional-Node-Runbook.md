# Platform Regional Node Runbook

**Purpose:** Define the operating expectations for multi-region and cross-network `styio-platform` deployment nodes.

**Last updated:** 2026-04-24

## Node Roles

Regional nodes may provide:

- compile worker capacity
- hosted workspace control-plane routing
- registry read mirrors
- registry write forwarding
- mirror synchronization and health reporting

Node roles must be explicit in deployment metadata so clients and operators know
whether a node is authoritative, read-only, write-forwarding, or compile-only.

## Cross-Network Deployment

Cross-network deployments must avoid assuming a single low-latency control
plane. Contracts should expose region, mirror freshness, accepted write origin,
and retry/replay behavior instead of hiding topology behind one opaque endpoint.

## Required Gates

Before a node role is promoted, validate:

- contract compatibility for hosted and registry APIs
- mirror freshness and replay behavior
- package read availability from the regional mirror
- worker-pool health for compile-capable nodes
- failure isolation between regions

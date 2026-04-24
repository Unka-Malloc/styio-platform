# Platform Control Plane V1 Contract

**Purpose:** Define the native JSON contract package for the first runnable `styio-platform` cloud service kernel.

**Last updated:** 2026-04-24

## Source Of Truth

This directory uses repo-native JSON contracts only:

- `platform-control-plane.contract.json`
- `platform-control-plane.examples.json`

Generated or third-party API description formats are not authoritative for
`styio-platform`. Gates validate these JSON contracts directly.

## Route Families

The V1 package covers health, node introspection, hosted job lifecycle,
worker-internal lifecycle, and mirror freshness/replay status. It is the route
surface implemented by the C++ `styio-platformd` kernel.

## Implementation Target

The first service kernel is intentionally small and runnable in one region:

- pure C++ service code
- Boost.Beast and Boost.Asio for the HTTP and async network layer
- Postgres for durable control-plane state, job lifecycle, and audit metadata
- provider-neutral object storage with S3 as the first backend
- mTLS for operator, regional-node, worker, and internal service traffic

Multi-region routing, mirror promotion, and provider fan-out must build on this
single-region kernel after the native JSON gates are executable.

# Platform Security Boundary

**Purpose:** Define the initial trust split for hosted compile and registry control-plane services.

**Last updated:** 2026-04-24

## Boundary

`styio-platform` treats local `styio-spio` manifests and lockfiles as client
inputs. It validates execution lanes, risk classes, source revisions, and
registry write requests before dispatching work to hosted workers or server
control planes.

Compiler-private execution remains behind `styio`; package-manager credential
storage remains in `styio-spio` until a platform credential service is designed.

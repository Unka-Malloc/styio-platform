# styio-platform Scripts

Platform scripts provide local server tools, native contract validation, stress
validation, external audit validation, and repository governance gates.

- `cloud-compile-stress.py` runs the deterministic compile-cloud stress harness.
- `registry-v2-control-plane-server.py` runs the local registry control-plane server.
- `audit-gate.sh` runs the external `styio-audit` gate for platform delivery.
- `docs-audit.py`, `docs-index.py`, `team-docs-gate.py`, and
  `repo-hygiene-gate.py` enforce docs and repository governance.

Contract validation now targets repo-native JSON contract and example packages
directly. Generated third-party API description maintenance is outside the
platform service-kernel path.

# styio-platform Scripts

Platform scripts provide local server tools, native contract validation, stress
validation, external audit validation, and repository governance gates.

- `cloud-compile-stress.py` runs the deterministic compile-cloud stress harness.
- `registry-v2-control-plane-server.py` runs the local registry control-plane server.
- `registry-v2-static-read-server.py` runs a read-only static registry read plane.
- `registry-v2-vm-smoke.py` validates a deployed VM registry node.
- `deploy-registry-vm.sh` installs the registry server bundle onto a Linux VM.
- `package-registry-server.sh` creates the VM deployment tarball.
- `audit-gate.sh` runs the external `styio-audit` gate for platform delivery.
- `docs-audit.py`, `docs-index.py`, `team-docs-gate.py`, and
  `repo-hygiene-gate.py` enforce docs and repository governance.

Contract validation now targets repo-native JSON contract and example packages
directly. Generated third-party API description maintenance is outside the
platform service-kernel path.

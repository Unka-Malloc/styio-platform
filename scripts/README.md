# styio-platform Scripts

Platform scripts provide local server tools, native contract validation, stress
validation, external audit validation, and repository governance gates.

- `cloud-compile-stress.py` runs the deterministic compile-cloud stress harness.
- `styio-platform` is the user-facing command wrapper for `dev up`, `dev status`,
  `dev join-workgroup`, `dev workgroup-status`, `dev shell`, `dev logs`, and
  `dev down`.
- `dev-env.py` implements the one-command local Kubernetes development
  environment lifecycle and local workgroup joining, with defaults loaded from
  `config/styio-platform-tools.yaml`.
- `styio_yaml.py` provides the repository-local YAML subset loader/writer for
  tool configuration and dev environment state without adding PyYAML.
- `registry-v2-control-plane-server.py` runs the local registry control-plane server.
- `registry-v2-static-read-server.py` runs a read-only static registry read plane.
- `registry-v2-vm-smoke.py` validates a deployed VM registry node.
- `publish-spio-tool-release.py` publishes a prebuilt tool executable such as
  `spio` or `styio` into a static read-plane root. `spio` is published under
  `tools/spio/`; `styio` client builds are published under release target
  namespaces such as `tools/styio-linux/`, `tools/styio-macos-cli/`,
  `tools/styio-macos-desktop-gui/`, `tools/styio-windows-cli/`,
  `tools/styio-windows-desktop-gui/`, `tools/styio-ios/`, or
  `tools/styio-android/`. The script writes shell-friendly channel pointers under
  `channel/<channel>/<platform>/version`, updates `latest.json` for API
  consumers, distinguishes `linux-*` and `linux-musl-*` platform keys when
  publishing from Linux build hosts, copies the installer script when provided, and rejects
  same-version same-platform artifact overwrite attempts with different
  content.
- `k8s-smoke.py` validates the Helm-based primary, worker, mirror, Postgres,
  and PVC deployment in a kind cluster with Podman/Buildah OCI tooling, using
  the same YAML tool configuration file for defaults.
- `deploy-registry-vm.sh` installs the registry server bundle onto a Linux VM.
- `package-registry-server.sh` creates the VM deployment tarball.
- `audit-gate.sh` runs the external `styio-audit` gate for platform delivery.
- `docs-audit.py`, `docs-index.py`, `team-docs-gate.py`, and
  `repo-hygiene-gate.py` enforce docs and repository governance.

Contract validation now targets repo-native JSON contract and example packages
directly. Generated third-party API description maintenance is outside the
platform service-kernel path.

Repository tools must use YAML for configuration files. JSON is still allowed
for wire/API contracts, request bodies, generated audit data, and other data
formats where JSON is the contract itself.

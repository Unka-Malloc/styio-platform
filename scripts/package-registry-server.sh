#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/package-registry-server.sh [options]

Create a VM deployment bundle for the styio-platform spio registry server.
The bundle contains install.sh, registry control/read servers, the registry v2
Python module, and smoke tooling. Copy the resulting tarball to a VM and run:

  tar -xzf styio-platform-registry-server-<version>.tar.gz
  cd styio-platform-registry-server-<version>
  sudo ./install.sh

Options:
  --version <value>     Package version label (default: current git short SHA)
  --output-dir <dir>    Output directory (default: dist)
  --name <value>        Package name (default: styio-platform-registry-server)
  --no-archive          Build the bundle directory without creating a tarball.
  -h, --help            Show this help.
USAGE
}

log() {
  echo "[registry-package] $*"
}

fail() {
  echo "[registry-package] $*" >&2
  exit 1
}

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

VERSION="$(git rev-parse --short HEAD 2>/dev/null || date -u +%Y%m%d%H%M%S)"
OUTPUT_DIR="dist"
PACKAGE_NAME="styio-platform-registry-server"
CREATE_ARCHIVE=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    --version)
      VERSION="$2"
      shift 2
      ;;
    --output-dir)
      OUTPUT_DIR="$2"
      shift 2
      ;;
    --name)
      PACKAGE_NAME="$2"
      shift 2
      ;;
    --no-archive)
      CREATE_ARCHIVE=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      fail "unknown option: $1"
      ;;
  esac
done

[[ "$VERSION" =~ ^[A-Za-z0-9._-]+$ ]] || fail "--version may only contain letters, numbers, dot, underscore, and dash"
[[ "$PACKAGE_NAME" =~ ^[A-Za-z0-9._-]+$ ]] || fail "--name may only contain letters, numbers, dot, underscore, and dash"

bundle_name="$PACKAGE_NAME-$VERSION"
bundle_dir="$OUTPUT_DIR/$bundle_name"
archive_path="$OUTPUT_DIR/$bundle_name.tar.gz"

rm -rf "$bundle_dir" "$archive_path"
install -d "$bundle_dir/scripts" "$bundle_dir/src/PlatformCloud" "$bundle_dir/docs"

install -m 0755 scripts/deploy-registry-vm.sh "$bundle_dir/install.sh"
install -m 0755 scripts/registry-v2-control-plane-server.py "$bundle_dir/scripts/registry-v2-control-plane-server.py"
install -m 0755 scripts/registry-v2-static-read-server.py "$bundle_dir/scripts/registry-v2-static-read-server.py"
install -m 0755 scripts/registry-v2-vm-smoke.py "$bundle_dir/scripts/registry-v2-vm-smoke.py"
cp -R src/PlatformCloud/PackageRegistry "$bundle_dir/src/PlatformCloud/PackageRegistry"
cp docs/operations/Spio-Registry-Server-Runbook.md "$bundle_dir/docs/Spio-Registry-Server-Runbook.md"

cat >"$bundle_dir/README.md" <<EOF
# Styio Platform Registry Server VM Bundle

Version: $VERSION

This bundle installs a spio registry v2 server node on a Linux VM. It creates:

- a registry control-plane service for publish, verify, and status
- a read-only static HTTP service for package metadata and artifacts
- initialized registry v2 metadata and signing keys
- a smoke check that validates status, verify, config.json, and trust/root.json

Install with:

\`\`\`text
sudo ./install.sh
\`\`\`

Safer internal default:

\`\`\`text
sudo ./install.sh --control-bind 127.0.0.1 --read-bind 0.0.0.0
\`\`\`

Manifest publish requests require a local spio binary. Pass it explicitly when
the VM does not provide /usr/local/bin/spio:

\`\`\`text
sudo ./install.sh --spio-bin /opt/spio/bin/spio
\`\`\`
EOF

cat >"$bundle_dir/MANIFEST.json" <<EOF
{
  "package": "$PACKAGE_NAME",
  "version": "$VERSION",
  "entrypoint": "install.sh",
  "services": [
    "styio-registry-control.service",
    "styio-registry-read.service"
  ],
  "control_plane_base_path": "/api/spio-registry-control/v1",
  "read_plane_protocol": "spio-static-registry",
  "read_plane_protocol_version": 2
}
EOF

if [[ "$CREATE_ARCHIVE" -eq 1 ]]; then
  tar -C "$OUTPUT_DIR" -czf "$archive_path" "$bundle_name"
  log "created $archive_path"
else
  log "created $bundle_dir"
fi

#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: install.sh [options]

Install a styio-platform spio registry server node on a Linux VM. The default
mode installs a local registry v2 root, initializes signing keys, writes two
systemd services, starts them, and runs a local smoke check.

Options:
  --install-dir <dir>     Runtime files directory (default: /opt/styio-platform-registry)
  --state-dir <dir>       Registry state root (default: /var/lib/styio-platform/registry)
  --config-dir <dir>      Environment file directory (default: /etc/styio-platform)
  --systemd-dir <dir>     systemd unit directory (default: /etc/systemd/system)
  --user <name>           Service user (default: styio-platform)
  --group <name>          Service group (default: same as --user)
  --registry-name <name>  Registry name (default: spio-registry-v2)
  --control-bind <addr>   Control-plane bind address (default: 127.0.0.1)
  --control-port <port>   Control-plane port (default: 8787)
  --read-bind <addr>      Static read-plane bind address (default: 0.0.0.0)
  --read-port <port>      Static read-plane port (default: 8788)
  --python <path>         Python 3 executable (default: first python3 in PATH)
  --spio-bin <path>       spio binary for manifest publish requests (default: /usr/local/bin/spio)
  --no-systemd            Install files and initialize state, but do not write or start units.
  --no-start              Write units, but do not enable/start services or run smoke.
  --skip-smoke            Start services without running the local smoke check.
  --force                 Overwrite runtime scripts and unit files.
  -h, --help              Show this help.
USAGE
}

log() {
  echo "[styio-registry-vm] $*"
}

fail() {
  echo "[styio-registry-vm] $*" >&2
  exit 1
}

require_command() {
  command -v "$1" >/dev/null 2>&1 || fail "required command not found: $1"
}

shell_quote() {
  printf '%q' "$1"
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -d "$script_dir/src/spio_registry_v2" && -f "$script_dir/scripts/registry-v2-control-plane-server.py" ]]; then
  bundle_root="$script_dir"
else
  bundle_root="$(cd "$script_dir/.." && pwd)"
fi

INSTALL_DIR="/opt/styio-platform-registry"
STATE_DIR="/var/lib/styio-platform/registry"
CONFIG_DIR="/etc/styio-platform"
SYSTEMD_DIR="/etc/systemd/system"
SERVICE_USER="styio-platform"
SERVICE_GROUP=""
REGISTRY_NAME="spio-registry-v2"
CONTROL_BIND="127.0.0.1"
CONTROL_PORT="8787"
READ_BIND="0.0.0.0"
READ_PORT="8788"
PYTHON_BIN="$(command -v python3 || true)"
SPIO_BIN="/usr/local/bin/spio"
WRITE_SYSTEMD=1
START_SERVICES=1
RUN_SMOKE=1
FORCE=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --install-dir)
      INSTALL_DIR="$2"
      shift 2
      ;;
    --state-dir)
      STATE_DIR="$2"
      shift 2
      ;;
    --config-dir)
      CONFIG_DIR="$2"
      shift 2
      ;;
    --systemd-dir)
      SYSTEMD_DIR="$2"
      shift 2
      ;;
    --user)
      SERVICE_USER="$2"
      shift 2
      ;;
    --group)
      SERVICE_GROUP="$2"
      shift 2
      ;;
    --registry-name)
      REGISTRY_NAME="$2"
      shift 2
      ;;
    --control-bind)
      CONTROL_BIND="$2"
      shift 2
      ;;
    --control-port)
      CONTROL_PORT="$2"
      shift 2
      ;;
    --read-bind)
      READ_BIND="$2"
      shift 2
      ;;
    --read-port)
      READ_PORT="$2"
      shift 2
      ;;
    --python)
      PYTHON_BIN="$2"
      shift 2
      ;;
    --spio-bin)
      SPIO_BIN="$2"
      shift 2
      ;;
    --no-systemd)
      WRITE_SYSTEMD=0
      START_SERVICES=0
      RUN_SMOKE=0
      shift
      ;;
    --no-start)
      START_SERVICES=0
      RUN_SMOKE=0
      shift
      ;;
    --skip-smoke)
      RUN_SMOKE=0
      shift
      ;;
    --force)
      FORCE=1
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

[[ -n "$PYTHON_BIN" ]] || fail "python3 was not found; pass --python <path>"
SERVICE_GROUP="${SERVICE_GROUP:-$SERVICE_USER}"
REGISTRY_ROOT="$STATE_DIR/v2"
KEY_DIR="$STATE_DIR/v2-keys"
ENV_FILE="$CONFIG_DIR/styio-registry.env"

[[ "$(id -u)" == "0" ]] || fail "run as root so the installer can create service users, state directories, and systemd units"
require_command "$PYTHON_BIN"
require_command openssl
if [[ "$WRITE_SYSTEMD" -eq 1 ]]; then
  require_command systemctl
fi

[[ -f "$bundle_root/scripts/registry-v2-control-plane-server.py" ]] || fail "bundle is missing scripts/registry-v2-control-plane-server.py"
[[ -f "$bundle_root/scripts/registry-v2-static-read-server.py" ]] || fail "bundle is missing scripts/registry-v2-static-read-server.py"
[[ -d "$bundle_root/src/spio_registry_v2" ]] || fail "bundle is missing src/spio_registry_v2"

if [[ "$FORCE" -ne 1 && -e "$INSTALL_DIR" && ! -d "$INSTALL_DIR/src/spio_registry_v2" ]]; then
  fail "$INSTALL_DIR exists but does not look like a styio registry install; pass --force to replace managed files"
fi

if ! getent group "$SERVICE_GROUP" >/dev/null 2>&1; then
  log "creating group $SERVICE_GROUP"
  groupadd --system "$SERVICE_GROUP"
fi
if ! id "$SERVICE_USER" >/dev/null 2>&1; then
  log "creating user $SERVICE_USER"
  useradd --system --gid "$SERVICE_GROUP" --home-dir "$STATE_DIR" --shell /usr/sbin/nologin "$SERVICE_USER"
fi

log "installing runtime files into $INSTALL_DIR"
install -d -m 0755 "$INSTALL_DIR/scripts" "$INSTALL_DIR/src" "$CONFIG_DIR" "$REGISTRY_ROOT" "$KEY_DIR"
rm -rf "$INSTALL_DIR/src/spio_registry_v2"
cp -R "$bundle_root/src/spio_registry_v2" "$INSTALL_DIR/src/spio_registry_v2"
install -m 0755 "$bundle_root/scripts/registry-v2-control-plane-server.py" "$INSTALL_DIR/scripts/registry-v2-control-plane-server.py"
install -m 0755 "$bundle_root/scripts/registry-v2-static-read-server.py" "$INSTALL_DIR/scripts/registry-v2-static-read-server.py"
install -m 0755 "$bundle_root/scripts/registry-v2-vm-smoke.py" "$INSTALL_DIR/scripts/registry-v2-vm-smoke.py"

log "writing $ENV_FILE"
cat >"$ENV_FILE" <<EOF
STYIO_REGISTRY_ROOT=$(shell_quote "$REGISTRY_ROOT")
STYIO_REGISTRY_KEY_DIR=$(shell_quote "$KEY_DIR")
STYIO_REGISTRY_NAME=$(shell_quote "$REGISTRY_NAME")
STYIO_REGISTRY_CONTROL_BIND=$(shell_quote "$CONTROL_BIND")
STYIO_REGISTRY_CONTROL_PORT=$(shell_quote "$CONTROL_PORT")
STYIO_REGISTRY_READ_BIND=$(shell_quote "$READ_BIND")
STYIO_REGISTRY_READ_PORT=$(shell_quote "$READ_PORT")
STYIO_REGISTRY_PYTHON=$(shell_quote "$PYTHON_BIN")
STYIO_REGISTRY_SPIO_BIN=$(shell_quote "$SPIO_BIN")
EOF
chmod 0640 "$ENV_FILE"
chown root:"$SERVICE_GROUP" "$ENV_FILE"

log "initializing registry root and signing keys"
PYTHONPATH="$INSTALL_DIR/src" "$PYTHON_BIN" - "$REGISTRY_ROOT" "$KEY_DIR" "$REGISTRY_NAME" <<'PY'
import pathlib
import sys
from spio_registry_v2 import initialize_registry_v2_root

registry_root = pathlib.Path(sys.argv[1])
key_dir = pathlib.Path(sys.argv[2])
registry_name = sys.argv[3]
initialize_registry_v2_root(str(registry_root), str(key_dir), registry_name=registry_name)
PY

chown -R "$SERVICE_USER":"$SERVICE_GROUP" "$STATE_DIR"
chmod 0750 "$STATE_DIR" "$REGISTRY_ROOT" "$KEY_DIR"

if [[ "$WRITE_SYSTEMD" -eq 1 ]]; then
  log "writing systemd units"
  control_unit="$SYSTEMD_DIR/styio-registry-control.service"
  read_unit="$SYSTEMD_DIR/styio-registry-read.service"
  cat >"$control_unit" <<EOF
[Unit]
Description=Styio spio registry v2 control plane
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
EnvironmentFile=$ENV_FILE
WorkingDirectory=$INSTALL_DIR
User=$SERVICE_USER
Group=$SERVICE_GROUP
ExecStart=$PYTHON_BIN $INSTALL_DIR/scripts/registry-v2-control-plane-server.py --root $REGISTRY_ROOT --key-dir $KEY_DIR --registry-name $REGISTRY_NAME --spio-bin $SPIO_BIN --bind $CONTROL_BIND --port $CONTROL_PORT
Restart=on-failure
RestartSec=3
NoNewPrivileges=true
PrivateTmp=true
ProtectHome=true
ProtectSystem=strict
ReadWritePaths=$REGISTRY_ROOT $KEY_DIR

[Install]
WantedBy=multi-user.target
EOF
  cat >"$read_unit" <<EOF
[Unit]
Description=Styio spio registry v2 static read plane
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
EnvironmentFile=$ENV_FILE
WorkingDirectory=$INSTALL_DIR
User=$SERVICE_USER
Group=$SERVICE_GROUP
ExecStart=$PYTHON_BIN $INSTALL_DIR/scripts/registry-v2-static-read-server.py --root $REGISTRY_ROOT --bind $READ_BIND --port $READ_PORT
Restart=on-failure
RestartSec=3
NoNewPrivileges=true
PrivateTmp=true
ProtectHome=true
ProtectSystem=strict
ReadOnlyPaths=$REGISTRY_ROOT

[Install]
WantedBy=multi-user.target
EOF
  chmod 0644 "$control_unit" "$read_unit"
  systemctl daemon-reload
fi

if [[ "$START_SERVICES" -eq 1 ]]; then
  log "enabling and starting registry services"
  systemctl enable --now styio-registry-control.service styio-registry-read.service
fi

if [[ "$RUN_SMOKE" -eq 1 ]]; then
  smoke_control_host="$CONTROL_BIND"
  smoke_read_host="$READ_BIND"
  [[ "$smoke_control_host" == "0.0.0.0" ]] && smoke_control_host="127.0.0.1"
  [[ "$smoke_read_host" == "0.0.0.0" ]] && smoke_read_host="127.0.0.1"
  log "running VM smoke check"
  "$PYTHON_BIN" "$INSTALL_DIR/scripts/registry-v2-vm-smoke.py" \
    --control-url "http://$smoke_control_host:$CONTROL_PORT" \
    --read-url "http://$smoke_read_host:$READ_PORT" \
    --json
fi

log "registry server deployment complete"
log "control plane: http://$CONTROL_BIND:$CONTROL_PORT/api/spio-registry-control/v1/status"
log "read plane: http://$READ_BIND:$READ_PORT/config.json"

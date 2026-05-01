#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/audit-gate.sh [options]

Run the external styio-audit gate against this repository.

Options:
  --audit-bin <path>  Explicit styio-audit executable
  -h, --help          Show this help
USAGE
}

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AUDIT_BIN="${STYIO_AUDIT_BIN:-}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --audit-bin)
      AUDIT_BIN="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "$AUDIT_BIN" ]]; then
  if [[ -x "$ROOT/../styio-audit/bin/styio-audit" ]]; then
    AUDIT_BIN="$ROOT/../styio-audit/bin/styio-audit"
  elif [[ -x "$HOME/eBioRing/styio-audit/bin/styio-audit" ]]; then
    AUDIT_BIN="$HOME/eBioRing/styio-audit/bin/styio-audit"
  elif [[ -x "/home/unka/styio-audit/bin/styio-audit" ]]; then
    AUDIT_BIN="/home/unka/styio-audit/bin/styio-audit"
  elif command -v styio-audit >/dev/null 2>&1; then
    AUDIT_BIN="$(command -v styio-audit)"
  fi
fi

if [[ -z "$AUDIT_BIN" || ! -x "$AUDIT_BIN" ]]; then
  echo "styio-audit executable not found; set STYIO_AUDIT_BIN or pass --audit-bin" >&2
  exit 2
fi

AUDIT_ROOT="$(cd "$(dirname "$AUDIT_BIN")/.." && pwd)"
if git -C "$AUDIT_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "styio-audit commit: $(git -C "$AUDIT_ROOT" rev-parse HEAD)"
fi

"$AUDIT_BIN" gate --repo "$ROOT" --project styio-platform

#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/delivery-gate.sh [options]

Run the common Styio platform delivery floor by composing repository hygiene,
the docs gate, native tests, and Python contract tests into one entrypoint.

Options:
  --mode <checkpoint|push>  Delivery mode (default: checkpoint)
  --base <ref>              Base ref for team-docs-gate branch checks
  --range <rev-range>       Explicit revision range for repo-hygiene push mode
  --skip-health             Skip native/Python health checks (docs/process-only deliveries)
  --build-dir <dir>         Build directory for CMake validation
  -h, --help                Show this help
USAGE
}

log() {
  echo "[delivery-gate] $*"
}

run_cmd() {
  log "$*"
  "$@"
}

default_upstream_base() {
  git rev-parse --abbrev-ref --symbolic-full-name '@{upstream}' 2>/dev/null || true
}

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

MODE="checkpoint"
BASE_REF=""
REV_RANGE=""
RUN_HEALTH=1
BUILD_DIR="build-codex"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode)
      MODE="$2"
      shift 2
      ;;
    --base)
      BASE_REF="$2"
      shift 2
      ;;
    --range)
      REV_RANGE="$2"
      shift 2
      ;;
    --skip-health)
      RUN_HEALTH=0
      shift
      ;;
    --build-dir)
      BUILD_DIR="$2"
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

REPO_CMD=(python3 scripts/repo-hygiene-gate.py)
DOCS_GATE_CMD=(./scripts/docs-gate.sh)
HEALTH_CMD=(bash -c "cmake -S . -B '$BUILD_DIR' -DSTYIO_PLATFORM_BUILD_TESTS=ON && cmake --build '$BUILD_DIR' && ctest --test-dir '$BUILD_DIR' --output-on-failure && python3 -m unittest tests/unit/test_cloud_compile_stress.py")

case "$MODE" in
  checkpoint)
    REPO_CMD+=(--mode staged)
    DOCS_GATE_CMD+=(--mode staged)
    ;;
  push)
    REPO_CMD+=(--mode push)
    if [[ -n "$REV_RANGE" ]]; then
      REPO_CMD+=(--range "$REV_RANGE")
    fi
    if [[ -z "$BASE_REF" ]]; then
      BASE_REF="$(default_upstream_base)"
    fi
    if [[ -z "$BASE_REF" ]]; then
      echo "push mode requires --base <ref> or a configured upstream branch" >&2
      exit 2
    fi
    DOCS_GATE_CMD+=(--mode push --base "$BASE_REF")
    ;;
  *)
    echo "Unsupported mode: $MODE" >&2
    usage >&2
    exit 2
    ;;
esac

run_cmd "${REPO_CMD[@]}"
run_cmd "${DOCS_GATE_CMD[@]}"

if [[ "$RUN_HEALTH" -eq 1 ]]; then
  run_cmd "${HEALTH_CMD[@]}"
else
  log "native/Python health checks skipped"
fi

log "all checks passed"

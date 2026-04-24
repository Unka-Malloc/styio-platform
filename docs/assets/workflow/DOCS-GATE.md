# Docs Gate

**Purpose:** Define the common docs/process gate entrypoint for `styio-platform` so contributors can run owner-runbook maintenance and docs audit through one command.

**Last updated:** 2026-04-19

## Command

Worktree docs gate:

```bash
./scripts/docs-gate.sh
```

Staged-only docs gate:

```bash
./scripts/docs-gate.sh --mode staged
```

Push or CI docs gate:

```bash
./scripts/docs-gate.sh --mode push --base origin/main
```

## What It Runs

1. `python3 scripts/team-docs-gate.py`
2. `python3 scripts/docs-audit.py`
`docs-audit.py` runs with `STYIO_SKIP_TEAM_DOC_GATE=1` when invoked through this entrypoint so the owner gate uses the requested change source exactly once.

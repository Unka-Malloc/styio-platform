# Delivery Gate

**Purpose:** Define the common delivery-floor entrypoint for `styio-platform` so contributors can run repository hygiene, the unified docs gate, native tests, and Python contract tests through one command before checkpoint merge or branch delivery.

**Last updated:** 2026-04-19

## Command

Checkpoint delivery floor:

```bash
./scripts/delivery-gate.sh --mode checkpoint
```

Push or branch-delivery floor:

```bash
./scripts/delivery-gate.sh --mode push --base origin/main
```

Docs/process-only delivery:

```bash
./scripts/delivery-gate.sh --mode checkpoint --skip-health
```

## What It Runs

1. `python3 scripts/repo-hygiene-gate.py`
2. `./scripts/docs-gate.sh`
3. CMake configure/build and `ctest`
4. `python3 -m unittest tests/unit/test_cloud_compile_stress.py`

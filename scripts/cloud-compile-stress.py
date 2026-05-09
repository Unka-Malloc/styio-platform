#!/usr/bin/env python3
from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVELOPER_WORKSPACE = ROOT / "src" / "PlatformCloud" / "DeveloperWorkspace"
if str(DEVELOPER_WORKSPACE) not in sys.path:
    sys.path.insert(0, str(DEVELOPER_WORKSPACE))

from workspace_compile_stress.cli import main  # noqa: E402


if __name__ == "__main__":
    raise SystemExit(main())

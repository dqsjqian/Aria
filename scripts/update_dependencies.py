#!/usr/bin/env python3
"""Update the project dependency lock; the next build consumes the new selection.

Examples: --only json; --version json=3.12.0.
Unspecified versions select latest stable; manifest versions remain authoritative.
Review the lock diff and run the normal build/tests before committing it.
"""
from pathlib import Path
import sys

from dependencies import main as resolve_main

ROOT = Path(__file__).resolve().parents[1]

if __name__ == "__main__":
    raise SystemExit(resolve_main([
        "update", "--manifest", str(ROOT / "dependencies.json"),
        "--lock", str(ROOT / "dependencies.lock.json"), *sys.argv[1:],
    ]))

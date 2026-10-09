#!/usr/bin/env python3
"""Unified build pipeline for Aria (Windows/macOS/Linux).

Complete pipeline: deps -> build -> test -> bench -> package.
Powered by aria_deps.build_kit (pip install aria-deps).

Usage:
    python scripts/build.py                 # Full pipeline: deps + build + test
    python scripts/build.py deps            # Only resolve dependencies
    python scripts/build.py build           # Only configure + compile
    python scripts/build.py test            # Only run tests
    python scripts/build.py bench           # Only run benchmarks
    python scripts/build.py package         # Only create release package
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    from aria_deps.build_kit import Pipeline
except ImportError:
    print("Error: aria-deps is required. Install it with:", file=sys.stderr)
    print("    pip install aria-deps", file=sys.stderr)
    sys.exit(1)

ROOT = Path(__file__).resolve().parents[1]


def extra_args(parser: argparse.ArgumentParser):
    parser.add_argument("--test", action="store_true", help="Run CTest after build")
    parser.add_argument("--offline", action="store_true", help="Offline dependency mode")
    parser.add_argument("--benchmark", action="store_true", help="Build benchmarks")


def deps_list(args) -> list:
    cmd = [sys.executable, str(ROOT / "scripts/dependencies.py"), "resolve",
           "--file", str(ROOT / "dependencies.json")]
    if args.offline:
        cmd.append("--offline")
    return [("deps", cmd)]


def cmake_flags(args) -> dict:
    return {
        "ARIA_DEPENDENCIES_OFFLINE": "ON" if args.offline else "OFF",
        "ARIA_BUILD_TESTS": "ON" if args.test else "OFF",
        "ARIA_BUILD_BENCHMARK": "ON" if args.benchmark else "OFF",
        "ARIA_BUILD_DOCS": "OFF",
    }


def main(argv=None) -> int:
    pipeline = Pipeline(
        name="aria",
        root=ROOT,
        deps=deps_list,
        cmake_flags=cmake_flags,
        extra_args=extra_args,
    )
    return pipeline.run(argv)


if __name__ == "__main__":
    sys.exit(main())

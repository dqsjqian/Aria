#!/usr/bin/env python3
"""Compatibility entry point for ForgeDeps; install requirements-build.txt first."""
from __future__ import annotations

import argparse
from pathlib import Path
import sys

try:
    from forgedeps.deps_build import (
        Context as _SharedContext, DependencyError, read_resolved, request_hash,
        resolve as _shared_resolve, validate_record,
    )
except ModuleNotFoundError as error:
    if error.name not in {"forgedeps", "forgedeps.deps_build"}:
        raise
    raise SystemExit("ForgeDeps is required: python -m pip install -r requirements-build.txt") from error


class Context(_SharedContext):
    def __init__(self, cache_dir, offline=False):
        super().__init__(cache_dir, offline, user_agent="aria-dependencies")


def resolve(file, effective_file=None, *, versions=None, only=None, update=False, context=None):
    return _shared_resolve(
        file, effective_file, versions=versions, only=only, update=update,
        context=context or Context(Path(file).parent / "build/deps/resolver"),
    )


def main(argv=None, *, command=None, file=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    if command is None:
        parser.add_argument("command", choices=("resolve", "update"))
    else:
        parser.set_defaults(command=command)
    parser.add_argument("--file", type=Path, default=file, required=file is None)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--cache-dir", "--cache", dest="cache_dir", type=Path)
    parser.add_argument("--version", action="append", default=[], metavar="NAME=VERSION")
    parser.add_argument("--only", action="append", metavar="NAME")
    parser.add_argument("--offline", action="store_true")
    args = parser.parse_args(argv)
    arguments = ["resolve", "--file", str(args.file)]
    if args.output:
        arguments += ["--output", str(args.output)]
    if args.cache_dir:
        arguments += ["--cache", str(args.cache_dir)]
    for name in args.only or ():
        arguments += ["--only", name]
    for version in args.version:
        arguments += ["--version", version]
    if args.command == "update":
        arguments.append("--update")
    if args.offline:
        arguments.append("--offline")
    from forgedeps.__main__ import main as shared_main
    return shared_main(arguments)


if __name__ == "__main__":
    raise SystemExit(main())

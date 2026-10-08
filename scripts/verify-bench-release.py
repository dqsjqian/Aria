#!/usr/bin/env python3
"""Reject draft, prerelease and non-release benchmark baselines."""
import json
import sys
from pathlib import Path


def verify(metadata, requested):
    if (metadata.get("tagName") != requested or metadata.get("isDraft") is not False
            or metadata.get("isPrerelease") is not False):
        raise ValueError("benchmark baseline must be the named published stable release")


if __name__ == "__main__":
    verify(json.loads(Path(sys.argv[1]).read_text(encoding="utf-8")), sys.argv[2])

#!/usr/bin/env python3
"""Record whether the historical absolute budget's calibration profile applies."""
from __future__ import annotations

import argparse
import json
import os
import platform
import subprocess
import sys
from pathlib import Path


def diagnose(facts: dict) -> list[str]:
    reasons = []
    if facts["system"] != "Darwin" or facts["machine"] != "arm64":
        reasons.append("historical calibration requires macOS ARM64")
    if facts.get("cpu") != "Apple M3 Pro" or "VMAPPLE" in facts["kernel"]:
        reasons.append("historical calibration requires a physical Apple M3 Pro")
    if "Apple clang version 21." not in facts.get("compiler", ""):
        reasons.append("historical calibration requires AppleClang 21")
    cache = facts.get("cache", {})
    if cache.get("CMAKE_BUILD_TYPE") != "Release":
        reasons.append("Release configuration is not verified")
    for sanitizer in ("ASAN", "UBSAN", "TSAN", "MSAN"):
        if cache.get(f"ARIA_ENABLE_{sanitizer}") != "OFF":
            reasons.append(f"{sanitizer} disabled state is not verified")
    expected_flags = {"CMAKE_CXX_FLAGS": "", "CMAKE_CXX_FLAGS_RELEASE": "-O3 -DNDEBUG"}
    for kind in ("EXE", "SHARED", "MODULE"):
        expected_flags[f"CMAKE_{kind}_LINKER_FLAGS"] = ""
        expected_flags[f"CMAKE_{kind}_LINKER_FLAGS_RELEASE"] = ""
    for key, expected in expected_flags.items():
        if cache.get(key) != expected:
            reasons.append(f"noncanonical or unverified Release flags: {key}")
    if cache.get("CMAKE_CXX_COMPILER_LAUNCHER") or cache.get("CMAKE_TOOLCHAIN_FILE"):
        reasons.append("custom compiler launcher/toolchain is not calibrated")
    if cache.get("ARIA_BENCH_CONTROL_STRETCH_PERCENT") != "0":
        reasons.append("benchmark delay control is enabled or not verified disabled")
    load = facts.get("load")
    if not load or not facts.get("cpu_count") or max(load) > facts["cpu_count"] * 0.5:
        reasons.append("load exceeds the declared half-logical-CPU idle-profile limit")
    return reasons


def capture(build: Path) -> dict:
    def command(args):
        result = subprocess.run(args, capture_output=True, timeout=10)
        return result.stdout.decode("utf-8").strip() if result.returncode == 0 else ""
    cache = {}
    cache_path = build / "CMakeCache.txt"
    if cache_path.is_file():
        for line in cache_path.read_text(encoding="utf-8").splitlines():
            if not line.startswith(("#", "//")) and ":" in line and "=" in line:
                key, value = line.split("=", 1)
                cache[key.split(":", 1)[0]] = value
    compiler = cache.get("CMAKE_CXX_COMPILER")
    return {"system": platform.system(), "machine": platform.machine(),
            "kernel": platform.version(), "cpu_count": os.cpu_count(),
            "load": list(os.getloadavg()) if hasattr(os, "getloadavg") else None,
            "cpu": command(["sysctl", "-n", "machdep.cpu.brand_string"])
            if platform.system() == "Darwin" else "",
            "compiler": command([compiler, "--version"]) if compiler else "",
            "cache": {key: value for key, value in cache.items()
                      if (key in ("CMAKE_BUILD_TYPE", "CMAKE_CXX_COMPILER_LAUNCHER", "CMAKE_TOOLCHAIN_FILE", "ARIA_BENCH_CONTROL_STRETCH_PERCENT")
                          or key.startswith("ARIA_ENABLE_") or "FLAGS" in key)}}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    args = parser.parse_args()
    facts = capture(args.build_dir)
    reasons = diagnose(facts)
    print("ARIA_BENCH_PROFILE " + json.dumps({"compatible": not reasons, "reasons": reasons,
                                            "facts": facts}, ensure_ascii=True))
    return 3 if reasons else 0


if __name__ == "__main__":
    sys.exit(main())

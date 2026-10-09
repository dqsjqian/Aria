#!/usr/bin/env python3
"""Smoke-test the real fixed-window CLI; these runs are not performance evidence."""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists() and (not args.output.is_dir() or any(args.output.iterdir())):
        parser.error("output must be a fresh or empty directory; existing evidence is retained")
    args.output.mkdir(parents=True, exist_ok=True)
    result = {"status": "invalid", "scope": "CLI smoke only; not performance qualification"}
    try:
        analysis = Path(__file__).with_name("compare-bench.py")
        spec = importlib.util.spec_from_file_location("scenario_smoke_compare", analysis)
        compare = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(compare)
        scenarios = ("list", "filtered", "sorted", "async")
        if set(compare.SCENARIOS) != set(scenarios) or len(compare.EXPECTED) != 4:
            raise ValueError("the smoke check requires all four canonical regression scenarios")
        binary = compare.bench_binary(args.bin_dir.resolve(), "aria_bench_regression")
        result.update({"protocol": compare.PROTOCOL,
                       "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                       "analysis_sha256": hashlib.sha256(analysis.read_bytes()).hexdigest(),
                       "valid_invocations": [], "invalid_invocations": []})
        for scenario in (None, *scenarios):
            selector = scenario or "all"
            log = args.output / f"{selector}.txt"
            rows = compare.measure(binary, log, scenario)
            expected = compare.EXPECTED if scenario is None else {
                compare.SCENARIOS[scenario]: compare.EXPECTED[compare.SCENARIOS[scenario]]}
            shape = {name: (row["samples"], row["ops"]) for name, row in rows.items()}
            if shape != expected or any(row["control_stretch_percent"] != 0 for row in rows.values()):
                raise ValueError(f"unexpected canonical fixture/control shape for {selector}")
            result["valid_invocations"].append({"scenario": selector, "metrics": list(rows),
                                                 "raw_log": log.name})
        invalid = [
            ["--scenario"], ["--scenario", "unknown"], ["--scenario", "all"],
            ["list"], ["--help"], ["--scenario", "list", "extra"],
            ["--scenario", "list", "--scenario", "list"],
            ["--scenario=list"], ["--unknown", "list"],
        ]
        for number, arguments in enumerate(invalid, 1):
            log = args.output / f"invalid-{number}.txt"
            try:
                process = subprocess.run([str(binary), *arguments], capture_output=True, timeout=30)
            except subprocess.TimeoutExpired as error:
                log.write_bytes((error.stdout or b"") + b"\n--- stderr ---\n" + (error.stderr or b""))
                raise RuntimeError(f"invalid CLI invocation timed out; see {log.name}") from error
            log.write_bytes(process.stdout + b"\n--- stderr ---\n" + process.stderr)
            if process.returncode != 2 or process.stdout:
                raise ValueError(f"invalid CLI arguments must exit 2 without measurements: {arguments}")
            result["invalid_invocations"].append({"argv": arguments, "exit_code": process.returncode,
                                                   "raw_log": log.name})
        result["status"] = "smoke-pass"
    except (OSError, ValueError, RuntimeError) as error:
        result["error"] = str(error)
        print(f"Scenario CLI smoke failed: {error}", file=sys.stderr)
    (args.output / "scenario-cli-smoke.json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if result["status"] == "smoke-pass":
        print("Scenario CLI smoke passed: default, four selectors and nine invalid argument sets")
        return 0
    return 2


if __name__ == "__main__":
    raise SystemExit(main())

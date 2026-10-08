#!/usr/bin/env python3
"""Qualify measurement with independent A-A / +20% delay controls, then compare.

Controls unavailable: exit 3, never candidate regression or success.
Candidate pass/regression/inconclusive: exit 0/1/3. Invalid setup: exit 2.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

BENCHES = ("aria_bench_regression", "aria_bench_iproperty", "aria_bench_command", "aria_bench_trace_sink")


def verify_controls(aa: dict, slow: dict) -> dict:
    for report in (aa, slow):
        if (report.get("rounds") != 64 or report.get("workload") != "ARIA_WORKLOAD fixed-window-v2 batch-mean-nearest-rank"
                or report.get("policy", {}).get("protocol") != "paired-median-v1"
                or len(report.get("summary", {})) != 24):
            raise ValueError("control report has wrong protocol, block count or metric coverage")
    if (set(aa["summary"]) != set(slow["summary"]) or aa.get("candidate_control_stretch_percent") != 0
            or slow.get("candidate_control_stretch_percent") != 20):
        raise ValueError("control workloads or injected delay identities differ")
    unavailable = {name: {"aa": aa["summary"][name]["status"], "slow20": slow["summary"][name]["status"]}
                   for name in aa["summary"] if aa["summary"][name]["status"] != "pass"
                   or slow["summary"][name]["status"] != "regression"}
    return {"status": "measurement-unavailable" if unavailable else "measurement-qualified",
            "unavailable_metrics": unavailable,
            "scope": "A-A noninferiority and +20% artificial wall-clock delay detection; not proof of iid or 10% algorithm sensitivity"}


def binary_hashes(directories: dict) -> dict:
    return {side: {bench: hashlib.sha256((directory / bench).read_bytes()).hexdigest() for bench in BENCHES}
            for side, directory in directories.items()}


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="strict")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--slow-control", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--policy", type=Path, default=Path(__file__).resolve().parents[1] / "benchmark/paired-policy.json")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    try:
        if (output / "validation.json").exists() or any((output / name).exists() for name in
                                                        ("aa-control", "slow20-control", "candidate")):
            raise ValueError("evidence directory already contains an attempt; use a fresh output directory")
        directories = {name: path.resolve() for name, path in
                       (("baseline", args.baseline), ("candidate", args.candidate), ("slow-control", args.slow_control))}
        initial_hashes = binary_hashes(directories)
        (output / "binary-sha256.json").write_text(json.dumps(initial_hashes, indent=2) + "\n", encoding="utf-8")
        # Freeze exact analysis code/policy before any new control measurement.
        script = output / "compare-bench-protocol.py"
        policy = output / "paired-policy.json"
        shutil.copyfile(Path(__file__).with_name("compare-bench.py"), script)
        shutil.copyfile(args.policy, policy)
        phases = []
        def phase(name: str, candidate: Path, stretch: int) -> dict:
            directory = output / name
            print(f"Starting {name}: fixed 64 blocks", flush=True)
            with (output / f"{name}.txt").open("wb") as log:
                result = subprocess.run([sys.executable, str(script), "--baseline", str(directories["baseline"]),
                                         "--candidate", str(candidate), "--output", str(directory),
                                         "--policy", str(policy), "--rounds", "64",
                                         "--expected-candidate-control", str(stretch)], stdout=log, stderr=subprocess.STDOUT)
            phases.append({"phase": name, "exit_code": result.returncode})
            (output / "phase-results.json").write_text(json.dumps(phases, indent=2) + "\n", encoding="utf-8")
            if binary_hashes(directories) != initial_hashes:
                raise ValueError("benchmark binaries changed during measurement")
            if result.returncode not in (0, 1, 3):
                raise RuntimeError(f"{name} failed to measure (exit {result.returncode}); see retained logs")
            return json.loads((directory / "comparison.json").read_text(encoding="utf-8"))
        aa = phase("aa-control", directories["baseline"], 0)
        slow = phase("slow20-control", directories["slow-control"], 20)
        qualification = verify_controls(aa, slow)
        result = {"controls": qualification, "candidate_status": "not-measured",
                  "status": qualification["status"], "exit_code": 3}
        if qualification["status"] == "measurement-qualified":
            candidate = phase("candidate", directories["candidate"], 0)
            result.update(candidate_status=candidate["status"], status=candidate["status"],
                          exit_code={"pass": 0, "regression": 1, "inconclusive": 3}[candidate["status"]])
        (output / "validation.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        summary = "### Benchmark measurement qualification\n\n" + json.dumps(result, indent=2) + "\n"
        print(summary)
        if os.environ.get("GITHUB_STEP_SUMMARY"):
            with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as target:
                target.write(summary)
        return result["exit_code"]
    except (OSError, ValueError, RuntimeError, KeyError) as error:
        (output / "validation-error.json").write_text(json.dumps({"status": "invalid", "error": str(error)}) + "\n",
                                                     encoding="utf-8")
        print(f"Measurement setup failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())

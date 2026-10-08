#!/usr/bin/env python3
"""Qualify measurement with independent A-A / +20% delay controls, then compare.

Controls unavailable: exit 3, never candidate regression or success.
Candidate pass/regression/inconclusive: exit 0/1/3. Invalid setup: exit 2.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import shutil
import subprocess
import sys
from pathlib import Path

BENCHES = ("aria_bench_regression", "aria_bench_iproperty", "aria_bench_command", "aria_bench_trace_sink")


BLOCK_PLAN = {bench: (64 if bench == "aria_bench_regression" else 512) for bench in BENCHES}


def expected_statistics() -> dict:
    # Deliberately independent of the comparison parser: qualification verifies
    # the exact reviewed acceptance surface, not just an arbitrary count of 24.
    names = {
        "aria_bench_regression": (
            "List append [1024,1224)", "Filtered append [10000,10200)",
            "Sorted random append [10000,10200)", "AsyncCommand round-trip [2 workers]"),
        "aria_bench_iproperty": (
            "Property<int>::set(i)", "IProperty::set_any(std::any{i})"),
        "aria_bench_command": (
            "Property<int>::set() + 1 observer", "Computed chain x5 (auto-tracked)"),
        "aria_bench_trace_sink": (
            "has_trace_sink() only (no sink)",
            "publish_trace gated, no sink (D-24 fast path)",
            "publish_trace ungated, no sink (AD2 anti-pattern)",
            "publish_trace gated, sink installed (slow path)"),
    }
    return {f"{name} / batch-{statistic}": {"bench": bench, "blocks": BLOCK_PLAN[bench]}
            for bench, metrics in names.items() for name in metrics for statistic in ("mean", "p99")}


EXPECTED_STATISTICS = expected_statistics()


def valid_number(value) -> bool:
    return (not isinstance(value, bool) and isinstance(value, (int, float))
            and math.isfinite(value) and value > 0)


def verify_controls(aa: dict, slow: dict) -> dict:
    for report in (aa, slow):
        policy = report.get("policy", {})
        if (report.get("macro_blocks") != 64 or report.get("blocks_by_bench") != BLOCK_PLAN
                or report.get("workload") != "ARIA_WORKLOAD fixed-window-v2 batch-mean-nearest-rank"
                or policy.get("protocol") != "paired-median-stratified-v2"
                or policy.get("family_alpha") != 0.05 or policy.get("family_comparisons") != 24
                or policy.get("ratio_limit") != 1.10 or policy.get("blocks_by_bench") != BLOCK_PLAN
                or set(report.get("summary", {})) != set(EXPECTED_STATISTICS)):
            raise ValueError("control report has wrong protocol, block plan, statistical policy or metric coverage")
        for name, shape in EXPECTED_STATISTICS.items():
            row = report["summary"][name]
            ranks = [20, 45] if shape["blocks"] == 64 else [221, 292]
            if (row.get("validity") != "fixed-sample-complete"
                    or row.get("bench") != shape["bench"] or row.get("observed_blocks") != shape["blocks"]
                    or row.get("required_blocks") != shape["blocks"]
                    or len(row.get("paired_ratios", [])) != shape["blocks"]
                    or row.get("order_statistic_ranks") != ranks
                    or row.get("status") not in ("pass", "inconclusive", "regression")):
                raise ValueError(f"control statistic has missing, duplicate or changed block evidence: {name}")
            ratios = row["paired_ratios"]
            intervals = row.get("paired_ratio_rounding_intervals", [])
            interval = row.get("ratio_interval", [])
            if (not all(valid_number(v) for v in ratios) or len(intervals) != shape["blocks"]
                    or any(not isinstance(pair, (list, tuple)) or len(pair) != 2
                           or not all(valid_number(v) for v in pair) or not pair[0] <= ratio <= pair[1]
                           for ratio, pair in zip(ratios, intervals))
                    or not isinstance(interval, (list, tuple)) or len(interval) != 2
                    or not all(valid_number(v) for v in interval) or interval[0] > interval[1]):
                raise ValueError(f"invalid paired ratio evidence: {name}")
            reconstructed = [sorted(pair[0] for pair in intervals)[ranks[0] - 1],
                             sorted(pair[1] for pair in intervals)[ranks[1] - 1]]
            if any(not math.isclose(a, b, rel_tol=1e-12, abs_tol=0) for a, b in zip(interval, reconstructed)):
                raise ValueError(f"control interval disagrees with paired ratio evidence: {name}")
            expected_status = ("pass" if interval[1] <= 1.10 else
                               "regression" if interval[0] > 1.10 else "inconclusive")
            if row["status"] != expected_status:
                raise ValueError(f"control status disagrees with its interval: {name}")
    if (aa.get("candidate_control_stretch_percent") != 0 or slow.get("candidate_control_stretch_percent") != 20
            or aa.get("comparison_kind") != "aa-control" or slow.get("comparison_kind") != "slow20-control"):
        raise ValueError("control identities differ from the independent A-A and real-delay design")
    unavailable = {name: {"aa": aa["summary"][name]["status"], "slow20": slow["summary"][name]["status"]}
                   for name in aa["summary"] if aa["summary"][name]["status"] != "pass"
                   or slow["summary"][name]["status"] != "regression"}
    return {"status": "measurement-unavailable" if unavailable else "measurement-qualified",
            "unavailable_metrics": unavailable,
            "scope": "A-A noninferiority and +20% artificial wall-clock delay detection; not proof of iid or 10% algorithm sensitivity"}


def protocol_hashes(paths: dict) -> dict:
    return {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in paths.items()}


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
    try:
        output.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        print(f"Invalid evidence directory: {error}", file=sys.stderr)
        return 2
    if any(output.iterdir()):
        # Preserve even an attempt interrupted before its first phase started.
        print("Evidence directory is not empty; use a fresh directory without overwriting an attempt", file=sys.stderr)
        return 2
    try:
        directories = {name: path.resolve() for name, path in
                       (("baseline", args.baseline), ("candidate", args.candidate), ("slow-control", args.slow_control))}
        initial_hashes = binary_hashes(directories)
        (output / "binary-sha256.json").write_text(json.dumps(initial_hashes, indent=2) + "\n", encoding="utf-8")
        # Freeze exact analysis code/policy before any new control measurement.
        script = output / "compare-bench-protocol.py"
        policy = output / "paired-policy.json"
        shutil.copyfile(Path(__file__).with_name("compare-bench.py"), script)
        shutil.copyfile(args.policy, policy)
        qualification_script = output / "qualification-runner-protocol.py"
        shutil.copyfile(Path(__file__), qualification_script)
        frozen_paths = {"analysis": script, "policy": policy, "qualification": qualification_script}
        initial_protocol_hashes = protocol_hashes(frozen_paths)
        (output / "protocol-sha256.json").write_text(json.dumps(initial_protocol_hashes, indent=2) + "\n",
                                                      encoding="utf-8")
        phases = []
        def phase(name: str, candidate: Path, stretch: int) -> dict:
            directory = output / name
            if protocol_hashes(frozen_paths) != initial_protocol_hashes:
                raise ValueError("frozen analysis, qualification code or policy changed before measurement")
            print(f"Starting {name}: fixed 64 heavy and 512 cheap blocks", flush=True)
            with (output / f"{name}.txt").open("wb") as log:
                result = subprocess.run([sys.executable, str(script), "--baseline", str(directories["baseline"]),
                                         "--candidate", str(candidate), "--output", str(directory),
                                         "--policy", str(policy), "--rounds", "64",
                                         "--expected-candidate-control", str(stretch)], stdout=log, stderr=subprocess.STDOUT)
            phases.append({"phase": name, "exit_code": result.returncode})
            (output / "phase-results.json").write_text(json.dumps(phases, indent=2) + "\n", encoding="utf-8")
            if binary_hashes(directories) != initial_hashes:
                raise ValueError("benchmark binaries changed during measurement")
            if protocol_hashes(frozen_paths) != initial_protocol_hashes:
                raise ValueError("frozen analysis, qualification code or policy changed during measurement")
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
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, AttributeError) as error:
        (output / "validation-error.json").write_text(json.dumps({"status": "invalid", "error": str(error)}) + "\n",
                                                     encoding="utf-8")
        print(f"Measurement setup failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())

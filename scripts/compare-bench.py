#!/usr/bin/env python3
"""Gate fixed-window batch means/P99 using complete-library same-host ABBA pairs.

Exit 0: every metric passes; 1: regression; 2: invalid input; 3: inconclusive.
Historical absolute budgets remain a separate result, never calibrated here.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import random
import re
import statistics
import subprocess
import sys
from pathlib import Path

BENCHES = ("aria_bench_regression", "aria_bench_iproperty", "aria_bench_command", "aria_bench_trace_sink")
LEGACY = {
    "aria_bench_iproperty": {"Property<int>::set(i)": (256, 4000),
                           "IProperty::set_any(std::any{i})": (256, 4000)},
    "aria_bench_command": {"Property<int>::set() + 1 observer": (256, 1000),
                         "Computed chain x5 (auto-tracked)": (128, 500)},
    "aria_bench_trace_sink": {name: (1000, 1000) for name in (
        "has_trace_sink() only (no sink)",
        "publish_trace gated, no sink (D-24 fast path)",
        "publish_trace ungated, no sink (AD2 anti-pattern)",
        "publish_trace gated, sink installed (slow path)")},
}
WORKLOAD = "ARIA_WORKLOAD fixed-window-v2 batch-mean-nearest-rank"
ROW = re.compile(
    r"^[RP]\s+(?P<name>.+?)\s+mean=\s*(?P<mean>\S+)ns\s+"
    r"p50=\s*(?P<p50>\S+)ns\s+p95=\s*(?P<p95>\S+)ns\s+"
    r"p99=\s*(?P<p99>\S+)ns\s+\((?P<samples>\d+)x(?P<ops>\d+)\)$"
)
EXPECTED = {
    "List append [1024,1224)": (1024, 200),
    "Filtered append [10000,10200)": (1024, 200),
    "Sorted random append [10000,10200)": (1024, 200),
    "AsyncCommand round-trip [2 workers]": (1024, 50),
}


def measure(binary: Path, log: Path) -> dict:
    try:
        result = subprocess.run([str(binary)], capture_output=True, timeout=600)
    except subprocess.TimeoutExpired as error:
        log.write_bytes((error.stdout or b"") + b"\n--- stderr ---\n" + (error.stderr or b""))
        raise RuntimeError(f"{binary.name} timed out; see {log.name}") from error
    # Preserve the original bytes, including non-UTF8 failure diagnostics.
    log.write_bytes(result.stdout + b"\n--- stderr ---\n" + result.stderr)
    if result.returncode:
        raise RuntimeError(f"{binary.name} exited with {result.returncode}; see {log.name}")
    output = result.stdout.decode("utf-8")
    expected = LEGACY.get(binary.name, EXPECTED)
    marker = "P " if binary.name in LEGACY else "R "
    if binary.name not in LEGACY and output.splitlines().count(WORKLOAD) != 1:
        raise ValueError("missing or duplicated fixed-window workload identity")
    controls = [line.removeprefix("C ARIA_BENCH_CONTROL stretch_percent=")
                for line in output.splitlines() if line.startswith("C ARIA_BENCH_CONTROL stretch_percent=")]
    if len(controls) != 1 or controls[0] not in ("0", "20"):
        raise ValueError("missing or invalid measurement control identity")
    control = int(controls[0])
    rows = {}
    samples = {}
    for line in output.splitlines():
        if line.startswith("S "):
            sample = json.loads(line[2:])
            name = sample["metric"]
            if name in samples:
                raise ValueError(f"duplicate batch samples: {name}")
            samples[name] = sample["batch_means_ns"]
            continue
        if not line.startswith(marker):
            continue
        match = ROW.fullmatch(line)
        if not match:
            raise ValueError(f"invalid measurement: {line}")
        name = match["name"].strip()
        if name in rows:
            raise ValueError(f"duplicate metric: {name}")
        values = {key: float(match[key]) for key in ("mean", "p50", "p95", "p99")}
        if any(not math.isfinite(value) or value <= 0 for value in values.values()):
            raise ValueError(f"non-finite or non-positive measurement: {name}")
        if not values["p50"] <= values["p95"] <= values["p99"]:
            raise ValueError(f"unordered percentiles: {name}")
        rows[name] = {**values, "samples": int(match["samples"]), "ops": int(match["ops"])}
    if {name: (row["samples"], row["ops"]) for name, row in rows.items()} != expected:
        raise ValueError("fixed-window workload mismatch (missing, extra or changed metric)")
    if set(samples) != set(rows):
        raise ValueError("missing fixed-window raw batch samples")
    for name, values in samples.items():
        if (not isinstance(values, list) or len(values) != rows[name]["samples"]
                or any(isinstance(value, bool) or not isinstance(value, (int, float))
                       or not math.isfinite(value) or value <= 0 for value in values)):
            raise ValueError(f"invalid raw batch samples: {name}")
        ordered = sorted(values)
        rebuilt = {"mean": statistics.mean(values)}
        rebuilt.update({key: ordered[math.ceil(q * len(values)) - 1]
                        for key, q in (("p50", 0.50), ("p95", 0.95), ("p99", 0.99))})
        for key, value in rebuilt.items():
            if abs(value - rows[name][key]) > max(0.0000011, abs(value) * 1e-12):
                raise ValueError(f"raw samples disagree with {key}: {name}")
        rows[name].update(rebuilt)
        rows[name]["rounding_radius_ns"] = 0.0000005
        rows[name]["control_stretch_percent"] = control
        rows[name]["batch_means_ns"] = values
    return rows


def median_interval_ranks(n: int, comparisons: int = 24, family_alpha: float = 0.05) -> tuple:
    """Exact two-sided binomial order-statistic interval for an iid median.

    Coverage is conditional on iid blocks, not proved by passing controls.
    Ties/discrete values are conservative; no observation is discarded.
    """
    choices = []
    for k in range(1, n // 2 + 1):
        tail = 2 * sum(math.comb(n, j) for j in range(k)) / 2 ** n
        if tail <= family_alpha / comparisons:
            choices.append((k, n - k + 1, 1 - comparisons * tail))
    if not choices:
        raise ValueError("too few blocks for the declared family confidence")
    return choices[-1]


def classify(blocks: list, limit: float, required_blocks: int = 64) -> dict:
    """Classify the population median of ABBA geometric ratios, not worst runs."""
    ratios, lower, upper, noise, envelopes = [], [], [], [], []
    for block in blocks:
        old, new = block["baseline"], block["candidate"]
        radius = block.get("rounding_radius_ns", 0.0)
        if min(*old, *new) <= radius:
            raise ValueError("measurement precision is insufficient for a positive ratio")
        log_ratio = (sum(math.log(v) for v in new) - sum(math.log(v) for v in old)) / 2
        ratios.append(math.exp(log_ratio))
        lower.append(math.exp((sum(math.log(v - radius) for v in new) -
                               sum(math.log(v + radius) for v in old)) / 2))
        upper.append(math.exp((sum(math.log(v + radius) for v in new) -
                               sum(math.log(v - radius) for v in old)) / 2))
        noise.append(max(old) / min(old))
        envelopes.append([min(new) / max(old), max(new) / min(old)])
    n = len(blocks)
    low_rank, high_rank, coverage = median_interval_ranks(n)
    low, high = sorted(lower)[low_rank - 1], sorted(upper)[high_rank - 1]
    sufficient = n == required_blocks
    status = ("inconclusive" if not sufficient else
              "pass" if high <= limit else
              "regression" if low > limit else "inconclusive")
    return {"status": status, "validity": "fixed-sample-complete" if sufficient else "wrong-sample-count",
            "paired_ratios": ratios, "baseline_repeat_ratios": noise, "block_envelopes": envelopes,
            "worst_case_envelope": [min(v[0] for v in envelopes), max(v[1] for v in envelopes)],
            "median_ratio": math.exp(statistics.median([math.log(v) for v in ratios])),
            "ratio_interval": [low, high], "order_statistic_ranks": [low_rank, high_rank],
            "joint_coverage_lower_bound_if_iid": coverage,
            "assumption": "independent identically distributed block ratios; controls do not prove iid"}


def compare(baseline: Path, candidate: Path, output: Path, rounds: int,
            limit: float = 1.10, expected_candidate_control: int = 0) -> dict:
    output.mkdir(parents=True, exist_ok=True)
    binaries = {"baseline": baseline.resolve(), "candidate": candidate.resolve()}
    expected_control = {"baseline": 0, "candidate": expected_candidate_control}
    runs = []
    def collect(side, bench, log):
        rows = measure(binaries[side] / bench, log)
        if any(row.get("control_stretch_percent", 0) != expected_control[side] for row in rows.values()):
            raise ValueError(f"unexpected measurement control on {side}: {bench}")
        # Raw ordered samples already live in the immutable executable log.
        # Keep summary/checkpoint JSON small to avoid quadratic I/O between blocks.
        for row in rows.values():
            row.pop("batch_means_ns", None)
        return rows
    for bench in BENCHES:
        for side in binaries:
            collect(side, bench, output / f"warmup-{side}-{bench}.txt")
    # Predeclared seed and balanced randomized orientation limit order confounding.
    # Randomization cannot establish stationarity or independence by itself.
    orientations = [False] * (rounds // 2) + [True] * (rounds - rounds // 2)
    random.Random(20261008).shuffle(orientations)
    with (output / "measurements.jsonl").open("w", encoding="utf-8") as checkpoint:
        for index, reversed_order in enumerate(orientations):
            order = ("candidate", "baseline", "baseline", "candidate") if reversed_order else (
                "baseline", "candidate", "candidate", "baseline")
            for bench in BENCHES:
                for position, side in enumerate(order, 1):
                    log = output / f"{index + 1}-{position}-{side}-{bench}.txt"
                    rows = collect(side, bench, log)
                    run = {"round": index + 1, "position": position, "side": side,
                           "bench": bench, "raw_log": log.name, "metrics": rows}
                    runs.append(run)
                    checkpoint.write(json.dumps(run) + "\n")
                    checkpoint.flush()
            (output / "progress.json").write_text(json.dumps({"completed_blocks": index + 1,
                                                              "fixed_total_blocks": rounds}) + "\n",
                                                  encoding="utf-8")
    summary = {}
    measured_names = {name for run in runs for name in run["metrics"]}
    for name in sorted(measured_names):
        for statistic in ("mean", "p99"):
            blocks = []
            for index in range(1, rounds + 1):
                selected = [run for run in runs if run["round"] == index and name in run["metrics"]]
                block = {side: [run["metrics"][name][statistic] for run in selected if run["side"] == side]
                         for side in binaries}
                block["rounding_radius_ns"] = max(run["metrics"][name].get("rounding_radius_ns", 0)
                                                 for run in selected)
                blocks.append(block)
            summary[f"{name} / batch-{statistic}"] = classify(blocks, limit)
    statuses = {row["status"] for row in summary.values()}
    status = "regression" if "regression" in statuses else (
        "inconclusive" if "inconclusive" in statuses else "pass")
    report = {"host": {"system": platform.system(), "machine": platform.machine(),
                       "platform": platform.platform(), "cpu_count": os.cpu_count()},
              "workload": WORKLOAD, "rounds": rounds, "runs": runs, "summary": summary,
              "comparison_kind": "slow20-control" if expected_candidate_control else (
                  "aa-control" if baseline.resolve() == candidate.resolve() else "candidate"),
              "candidate_control_stretch_percent": expected_candidate_control,
              "status": status, "policy": {"ratio_limit": limit, "protocol": "paired-median-v1",
              "fixed_blocks": 64, "family_alpha": 0.05, "family_comparisons": 24,
              "aggregation": "exact binomial order-statistic interval for the median ABBA geometric ratio",
              "scope": "median repeated-run batch mean and batch-mean P99, not individual-operation latency"}}
    (output / "comparison.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="strict")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=64)
    parser.add_argument("--expected-candidate-control", type=int, choices=(0, 20), default=0)
    parser.add_argument("--policy", type=Path, default=Path(__file__).resolve().parents[1] /
                        "benchmark/paired-policy.json")
    args = parser.parse_args()
    if args.rounds != 64:
        parser.error("--rounds must equal the fixed preregistered 64-block design")
    args.output.mkdir(parents=True, exist_ok=True)
    try:
        policy = json.loads(args.policy.read_text(encoding="utf-8"))
        if (not isinstance(policy, dict) or policy.get("workload") != "fixed-window-v2"
                or policy.get("fixed_blocks") != 64
                or policy.get("family_alpha") != 0.05 or policy.get("family_comparisons") != 24
                or policy.get("protocol") != "paired-median-v1"
                or policy.get("order_seed") != 20261008
                or policy.get("candidate_ratio_limit") != 1.10):
            raise ValueError("policy/workload identity or minimum block count mismatch")
        limit = policy["candidate_ratio_limit"]
        if any(isinstance(value, bool) or not isinstance(value, (int, float)) or
               not math.isfinite(value) or value < 1 for value in (limit,)):
            raise ValueError("invalid paired ratio policy")
        provenance = {side: {bench: hashlib.sha256((directory / bench).read_bytes()).hexdigest()
                            for bench in BENCHES}
                      for side, directory in (("baseline", args.baseline), ("candidate", args.candidate))}
        (args.output / "binary-sha256.json").write_text(json.dumps(provenance, indent=2) + "\n", encoding="utf-8")
        report = compare(args.baseline, args.candidate, args.output, args.rounds, limit, args.expected_candidate_control)
        lines = [f"### Paired median protocol: {report['comparison_kind']}", "", f"Result: **{report['status']}**", "",
                 "| metric | median-ratio interval (iid assumption) | worst baseline repeat ratio | result |",
                 "|---|---:|---:|---|"]
        for name, row in report["summary"].items():
            low, high = row["ratio_interval"]
            lines.append(f"| {name} | {low:.3f} - {high:.3f} | "
                         f"{max(row['baseline_repeat_ratios']):.3f} | {row['status']} |")
        summary = "\n".join(lines) + "\n"
        print(summary)
        if os.environ.get("GITHUB_STEP_SUMMARY"):
            with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as target:
                target.write(summary)
        return {"pass": 0, "regression": 1, "inconclusive": 3}[report["status"]]
    except (OSError, ValueError, RuntimeError, KeyError) as error:
        (args.output / "error.json").write_text(json.dumps({"status": "invalid", "error": str(error)}) +
                                                "\n", encoding="utf-8")
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())

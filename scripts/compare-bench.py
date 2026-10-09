#!/usr/bin/env python3
"""Gate fixed-window batch means/P99 using complete-library same-host ABBA pairs.

Exit 0: every metric passes; 1: regression; 2: invalid input; 3: inconclusive.
Historical absolute budgets remain a separate result, never calibrated here.
"""
from __future__ import annotations

import argparse
import datetime
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
import time
from pathlib import Path

BENCHES = ("aria_bench_regression", "aria_bench_iproperty", "aria_bench_command", "aria_bench_trace_sink")
PROTOCOL = "paired-median-scenario-v3"
ORDER_SEED = 20261009
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
SCENARIOS = dict(zip(("list", "filtered", "sorted", "async"), EXPECTED))
SUITES = {f"aria_bench_regression--{scenario}": {"bench": BENCHES[0], "scenario": scenario}
          for scenario in SCENARIOS}
SUITES.update({bench: {"bench": bench, "scenario": None} for bench in BENCHES[1:]})
BLOCK_PLAN = {suite: (64 if shape["scenario"] else 512) for suite, shape in SUITES.items()}


def bench_binary(directory: Path, name: str) -> Path:
    """Resolve one explicit build output without guessing between stale files."""
    choices = [path for path in (directory / name, directory / f"{name}.exe") if path.is_file()]
    if len(choices) != 1:
        raise ValueError(f"expected exactly one benchmark executable for {name} in {directory}; found {len(choices)}")
    return choices[0]


def measure(binary: Path, log: Path, scenario: str | None = None) -> dict:
    name = binary.stem if binary.suffix.lower() == ".exe" else binary.name
    if name not in BENCHES or (scenario is not None and (name != BENCHES[0] or scenario not in SCENARIOS)):
        raise ValueError("unknown benchmark executable or scenario selector")
    command = [str(binary)] + (["--scenario", scenario] if scenario is not None else [])
    try:
        result = subprocess.run(command, capture_output=True, timeout=600)
    except subprocess.TimeoutExpired as error:
        log.write_bytes((error.stdout or b"") + b"\n--- stderr ---\n" + (error.stderr or b""))
        raise RuntimeError(f"{binary.name} timed out; see {log.name}") from error
    # Preserve the original bytes, including non-UTF8 failure diagnostics.
    log.write_bytes(result.stdout + b"\n--- stderr ---\n" + result.stderr)
    if result.returncode:
        raise RuntimeError(f"{binary.name} exited with {result.returncode}; see {log.name}")
    output = result.stdout.decode("utf-8")
    # Windows adds .exe to the same logical benchmark name. The suffix must
    # not turn a legacy P workload into an unknown R workload.
    expected = LEGACY[name] if name in LEGACY else (
        {SCENARIOS[scenario]: EXPECTED[SCENARIOS[scenario]]} if scenario is not None else EXPECTED)
    marker = "P " if name in LEGACY else "R "
    if name not in LEGACY and output.splitlines().count(WORKLOAD) != 1:
        raise ValueError("missing or duplicated fixed-window workload identity")
    selectors = [line for line in output.splitlines() if line.startswith("ARIA_SCENARIO")]
    if selectors != ([] if name in LEGACY else [f"ARIA_SCENARIO {scenario or 'all'}"]):
        raise ValueError("missing, duplicated or mismatched scenario identity")
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
        if line.startswith(("R ", "P ")) and not line.startswith(marker):
            raise ValueError("unexpected measurement row family")
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
    cumulative = 0
    for k in range(1, n // 2 + 1):
        cumulative += math.comb(n, k - 1)
        tail = 2 * cumulative / 2 ** n
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
    return {"status": status, "observed_blocks": n, "required_blocks": required_blocks,
            "marginal_noncoverage_bound_if_iid": (1 - coverage) / 24, "validity": "fixed-sample-complete" if sufficient else "wrong-sample-count",
            "paired_ratios": ratios, "paired_ratio_rounding_intervals": list(zip(lower, upper)),
            "baseline_repeat_ratios": noise, "block_envelopes": envelopes,
            "worst_case_envelope": [min(v[0] for v in envelopes), max(v[1] for v in envelopes)],
            "median_ratio": math.exp(statistics.median([math.log(v) for v in ratios])),
            "ratio_interval": [low, high], "order_statistic_ranks": [low_rank, high_rank],
            "marginal_coverage_lower_bound_if_iid": 1 - (1 - coverage) / 24,
            "assumption": "independent identically distributed block ratios; controls do not prove iid"}


def expected_statistics() -> dict:
    return {f"{name} / batch-{statistic}": {**shape, "suite": suite, "blocks": BLOCK_PLAN[suite]}
            for suite, shape in SUITES.items()
            for name in ([SCENARIOS[shape["scenario"]]] if shape["scenario"] else LEGACY[shape["bench"]])
            for statistic in ("mean", "p99")}


def make_schedule() -> list:
    """Freeze 64 macros spanning both cost families; all orders fixed in advance.

    More densely spaced cheap blocks may cluster. This schedule and randomized
    orientation do not prove iid or justify an effective independent sample size.
    """
    orientations = {}
    for suite, count in BLOCK_PLAN.items():
        values = [False] * (count // 2) + [True] * (count // 2)
        random.Random(f"{ORDER_SEED}:orientation:{suite}").shuffle(values)
        orientations[suite] = values
    placement = random.Random(f"{ORDER_SEED}:macro-placement")
    bench_order = random.Random(f"{ORDER_SEED}:cheap-bench-order")
    counters = dict.fromkeys(SUITES, 0)
    heavy = [suite for suite, shape in SUITES.items() if shape["scenario"]]
    cheap = [suite for suite, shape in SUITES.items() if not shape["scenario"]]
    schedule = []
    for macro in range(1, 65):
        slots = heavy + [None] * 8
        placement.shuffle(slots)
        for slot, heavy_suite in enumerate(slots, 1):
            suites = [heavy_suite] if heavy_suite is not None else list(cheap)
            if heavy_suite is None:
                bench_order.shuffle(suites)
            for suite in suites:
                counters[suite] += 1
                block = counters[suite]
                reversed_order = orientations[suite][block - 1]
                order = ["candidate", "baseline", "baseline", "candidate"] if reversed_order else [
                    "baseline", "candidate", "candidate", "baseline"]
                schedule.append({"sequence": len(schedule) + 1, "macro": macro,
                                 "macro_slot": slot, "family": "heavy" if heavy_suite is not None else "cheap",
                                 **SUITES[suite], "suite": suite,
                                 "block": block, "order": order})
    assert counters == BLOCK_PLAN
    return schedule


def compare(baseline: Path, candidate: Path, output: Path, rounds: int,
            limit: float = 1.10, expected_candidate_control: int = 0) -> dict:
    if rounds != 64:
        raise ValueError("the fixed design requires exactly 64 macro blocks")
    output.mkdir(parents=True, exist_ok=True)
    if any((output / name).exists() for name in ("schedule.json", "measurements.jsonl", "comparison.json")):
        raise ValueError("evidence directory already contains an attempt; use a fresh directory")
    binaries = {"baseline": baseline.resolve(), "candidate": candidate.resolve()}
    executables = {side: {bench: bench_binary(directory, bench) for bench in BENCHES}
                   for side, directory in binaries.items()}
    identities = {side: {bench: hashlib.sha256(binary.read_bytes()).hexdigest()
                         for bench, binary in selected.items()} for side, selected in executables.items()}
    manifest = output / "binary-sha256.json"
    manifest_bytes = (json.dumps(identities, indent=2) + "\n").encode()
    if manifest.exists():
        manifest_bytes = manifest.read_bytes()
        if json.loads(manifest_bytes) != identities:
            raise ValueError("phase executable manifest differs before measurement")
    else:
        manifest.write_text(json.dumps(identities, indent=2) + "\n", encoding="utf-8")
    provenance = {"phase_executables": {"file": manifest.name,
                                      "sha256": hashlib.sha256(manifest_bytes).hexdigest()},
                  "analysis_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    # Qualification freezes the complete project-library and analysis/policy
    # identities outside each phase. Every invocation explicitly joins them.
    # A standalone comparison has no campaign manifest and cannot qualify it.
    for key, filename in (("campaign_binaries_and_libraries", "binary-sha256.json"),
                          ("campaign_protocol", "protocol-sha256.json")):
        source = output.parent / filename
        provenance[key] = ({"file": "../" + filename,
                            "sha256": hashlib.sha256(source.read_bytes()).hexdigest()}
                           if source.is_file() else None)
    expected_control = {"baseline": 0, "candidate": expected_candidate_control}
    runs = []
    grouped = {}
    schedule = make_schedule()
    (output / "schedule.json").write_text(json.dumps(schedule, indent=2) + "\n", encoding="utf-8")
    def collect(side, suite, log):
        shape = SUITES[suite]
        rows = measure(executables[side][shape["bench"]], log, scenario=shape["scenario"])
        if any(row.get("control_stretch_percent", 0) != expected_control[side] for row in rows.values()):
            raise ValueError(f"unexpected measurement control on {side}: {suite}")
        for row in rows.values():
            row.pop("batch_means_ns", None)
        return rows
    warmups = []
    with (output / "warmups.jsonl").open("w", encoding="utf-8") as checkpoint:
        for suite, shape in SUITES.items():
            for side in binaries:
                log = output / f"warmup-{side}-{suite}.txt"
                warmup = {**shape, "suite": suite, "side": side, "raw_log": log.name,
                          "provenance": provenance,
                          "started_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                          "monotonic_start_ns": time.monotonic_ns()}
                checkpoint.write(json.dumps({**warmup, "event": "started"}) + "\n")
                checkpoint.flush()
                try:
                    collect(side, suite, log)
                except Exception as error:
                    checkpoint.write(json.dumps({**warmup, "event": "failed", "error": str(error),
                                                 "monotonic_end_ns": time.monotonic_ns()}) + "\n")
                    checkpoint.flush()
                    raise
                warmup.update(monotonic_end_ns=time.monotonic_ns(),
                              completed_at_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
                warmups.append(warmup)
                checkpoint.write(json.dumps({**warmup, "event": "completed"}) + "\n")
                checkpoint.flush()
    completed = dict.fromkeys(SUITES, 0)
    with (output / "measurements.jsonl").open("w", encoding="utf-8") as checkpoint:
        for item in schedule:
            suite = item["suite"]
            for position, side in enumerate(item["order"], 1):
                log = output / f"{item['sequence']}-{position}-{side}-{suite}.txt"
                run = {**{key: value for key, value in item.items() if key != "order"},
                       "position": position, "side": side, "raw_log": log.name,
                       "provenance": provenance,
                       "started_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                       "monotonic_start_ns": time.monotonic_ns()}
                checkpoint.write(json.dumps({**run, "event": "started"}) + "\n")
                checkpoint.flush()
                try:
                    rows = collect(side, suite, log)
                except Exception as error:
                    checkpoint.write(json.dumps({**run, "event": "failed", "error": str(error),
                                                 "monotonic_end_ns": time.monotonic_ns()}) + "\n")
                    checkpoint.flush()
                    raise
                run.update(metrics=rows, monotonic_end_ns=time.monotonic_ns(),
                           completed_at_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
                runs.append(run)
                grouped.setdefault((suite, item["block"]), []).append(run)
                checkpoint.write(json.dumps({**run, "event": "completed"}) + "\n")
                checkpoint.flush()
            completed[suite] += 1
            (output / "progress.json").write_text(json.dumps({"current_macro": item["macro"],
                        "completed_blocks_by_suite": completed, "fixed_blocks_by_suite": BLOCK_PLAN}) + "\n",
                        encoding="utf-8")
    summary = {}
    for key, shape in expected_statistics().items():
        name, statistic = key.rsplit(" / batch-", 1)
        blocks = []
        for index in range(1, shape["blocks"] + 1):
            selected = grouped.get((shape["suite"], index), [])
            block = {side: [run["metrics"][name][statistic] for run in selected if run["side"] == side]
                     for side in binaries}
            if any(len(values) != 2 for values in block.values()):
                raise ValueError("missing or duplicated run within a paired block")
            block["rounding_radius_ns"] = max(run["metrics"][name].get("rounding_radius_ns", 0)
                                             for run in selected)
            blocks.append(block)
        summary[key] = {**classify(blocks, limit, required_blocks=shape["blocks"]),
                        **{field: shape[field] for field in ("bench", "suite", "scenario")}}
    statuses = {row["status"] for row in summary.values()}
    status = "regression" if "regression" in statuses else (
        "inconclusive" if "inconclusive" in statuses else "pass")
    report = {"host": {"system": platform.system(), "machine": platform.machine(),
                       "platform": platform.platform(), "cpu_count": os.cpu_count(),
                       "python_version": sys.version},
              "workload": WORKLOAD, "macro_blocks": rounds, "blocks_by_suite": BLOCK_PLAN,
              "warmups": warmups, "runs": runs, "summary": summary,
              "joint_coverage_lower_bound_if_iid": 1 - sum(row["marginal_noncoverage_bound_if_iid"] for row in summary.values()),
              "comparison_kind": "slow20-control" if expected_candidate_control else (
                  "aa-control" if baseline.resolve() == candidate.resolve() else "candidate"),
              "candidate_control_stretch_percent": expected_candidate_control,
              "status": status, "policy": {"ratio_limit": limit, "protocol": PROTOCOL,
              "blocks_by_suite": BLOCK_PLAN, "family_alpha": 0.05, "family_comparisons": 24,
              "aggregation": "exact binomial order-statistic interval for the median ABBA geometric ratio",
              "scope": "median repeated-run batch mean and batch-mean P99, not individual-operation latency",
              "assumptions": "iid ratios within each statistic; cheap blocks may cluster within macros; controls and counts do not prove iid"}}
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
        parser.error("--rounds must equal the fixed preregistered 64-macro design")
    args.output.mkdir(parents=True, exist_ok=True)
    if any(args.output.iterdir()):
        parser.error("evidence directory is not empty; use a fresh directory without overwriting an attempt")
    try:
        policy = json.loads(args.policy.read_text(encoding="utf-8"))
        if (not isinstance(policy, dict) or policy.get("workload") != "fixed-window-v2"
                or policy.get("macro_blocks") != 64 or policy.get("blocks_by_suite") != BLOCK_PLAN
                or policy.get("family_alpha") != 0.05 or policy.get("family_comparisons") != 24
                or policy.get("protocol") != PROTOCOL
                or policy.get("order_seed") != ORDER_SEED
                or policy.get("candidate_ratio_limit") != 1.10):
            raise ValueError("policy/workload identity or minimum block count mismatch")
        limit = policy["candidate_ratio_limit"]
        if any(isinstance(value, bool) or not isinstance(value, (int, float)) or
               not math.isfinite(value) or value < 1 for value in (limit,)):
            raise ValueError("invalid paired ratio policy")
        provenance = {side: {bench: hashlib.sha256(bench_binary(directory, bench).read_bytes()).hexdigest()
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
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, AttributeError) as error:
        (args.output / "error.json").write_text(json.dumps({"status": "invalid", "error": str(error)}) +
                                                "\n", encoding="utf-8")
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())

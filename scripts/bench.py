#!/usr/bin/env python3
"""Aria benchmark toolchain in one entry.

Subcommands:
  profile        Record whether the historical absolute budget's calibration profile applies.
  check          Run the suite and fail if any batch-mean P99 exceeds the pinned ceiling.
  scenarios      Smoke-test the real fixed-window CLI; not performance evidence.
  compare        Paired-median fixed-window regression comparison (protocol v3.1).
  validate       Three-phase qualification campaign (A-A, +20% delay, candidate).
  verify-release Reject draft, prerelease and non-release benchmark baselines.

Historical absolute budgets remain separate from the paired gate; exit codes
match the previous per-tool contracts (0 pass, 1 regression, 2 invalid, 3
unavailable/inconclusive).
"""
from __future__ import annotations

import argparse
import contextlib
import datetime
import hashlib
import io
import json
import math
import os
import platform
import random
import re
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

# ══ compare: paired-median protocol v3.1 ═════════════════════════════════════

BENCHES = ("aria_bench_regression", "aria_bench_iproperty", "aria_bench_command", "aria_bench_trace_sink")
PROTOCOL = "paired-median-scenario-v3.1"
DECISION_STATISTICS = ("batch-mean",)
OBSERVED_STATISTICS = ("batch-p99",)
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
        role = "decision" if f"batch-{statistic}" in DECISION_STATISTICS else "observed"
        summary[key] = {**classify(blocks, limit, required_blocks=shape["blocks"]),
                        "role": role,
                        **{field: shape[field] for field in ("bench", "suite", "scenario")}}
    statuses = {row["status"] for key, row in summary.items()
                if key.endswith(tuple(f" / {statistic}" for statistic in DECISION_STATISTICS))}
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
              "decision_statistics": list(DECISION_STATISTICS),
              "observed_statistics": list(OBSERVED_STATISTICS),
              "aggregation": "exact binomial order-statistic interval for the median ABBA geometric ratio",
              "scope": "median repeated-run batch mean and batch-mean P99, not individual-operation latency",
              "assumptions": "iid ratios within each statistic; cheap blocks may cluster within macros; controls and counts do not prove iid"}}
    (output / "comparison.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def compare_main(argv=None) -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="strict")
    parser = argparse.ArgumentParser(description="Gate fixed-window batch means/P99 using complete-library same-host ABBA pairs. Exit 0: every decision metric passes; 1: regression; 2: invalid input; 3: inconclusive.")
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=64)
    parser.add_argument("--expected-candidate-control", type=int, choices=(0, 20), default=0)
    parser.add_argument("--policy", type=Path, default=Path(__file__).resolve().parents[1] /
                        "benchmark/paired-policy.json")
    args = parser.parse_args(argv)
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
                or policy.get("candidate_ratio_limit") != 1.10
                or policy.get("decision_statistics") != list(DECISION_STATISTICS)
                or policy.get("observed_statistics") != list(OBSERVED_STATISTICS)):
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
            verdict = row["status"] + ("" if row["role"] == "decision" else " (observed)")
            lines.append(f"| {name} | {low:.3f} - {high:.3f} | "
                         f"{max(row['baseline_repeat_ratios']):.3f} | {verdict} |")
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

# ══ validate: three-phase qualification ═════════════════════════════════════

PROTOCOL = "paired-median-scenario-v3.1"
DECISION_SUFFIX = " / batch-mean"
SCENARIOS = {
    "list": "List append [1024,1224)",
    "filtered": "Filtered append [10000,10200)",
    "sorted": "Sorted random append [10000,10200)",
    "async": "AsyncCommand round-trip [2 workers]",
}
SUITES = {f"aria_bench_regression--{scenario}": {"bench": BENCHES[0], "scenario": scenario}
          for scenario in SCENARIOS}
SUITES.update({bench: {"bench": bench, "scenario": None} for bench in BENCHES[1:]})
BLOCK_PLAN = {suite: (64 if shape["scenario"] else 512) for suite, shape in SUITES.items()}


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
    return {f"{name} / batch-{statistic}": {**shape, "suite": suite, "blocks": BLOCK_PLAN[suite]}
            for suite, shape in SUITES.items()
            for name in ([SCENARIOS[shape["scenario"]]] if shape["scenario"] else names[shape["bench"]])
            for statistic in ("mean", "p99")}


EXPECTED_STATISTICS = expected_statistics()


def valid_number(value) -> bool:
    return (not isinstance(value, bool) and isinstance(value, (int, float))
            and math.isfinite(value) and value > 0)


def verify_statistics(report: dict) -> str:
    policy = report.get("policy", {})
    if (report.get("macro_blocks") != 64 or report.get("blocks_by_suite") != BLOCK_PLAN
            or report.get("workload") != "ARIA_WORKLOAD fixed-window-v2 batch-mean-nearest-rank"
            or policy.get("protocol") != PROTOCOL
            or policy.get("family_alpha") != 0.05 or policy.get("family_comparisons") != 24
            or policy.get("ratio_limit") != 1.10 or policy.get("blocks_by_suite") != BLOCK_PLAN
            or set(report.get("summary", {})) != set(EXPECTED_STATISTICS)):
        raise ValueError("statistical report has wrong protocol, block plan, statistical policy or metric coverage")
    for name, shape in EXPECTED_STATISTICS.items():
        row = report["summary"][name]
        ranks = [20, 45] if shape["blocks"] == 64 else [221, 292]
        role = "decision" if name.endswith(DECISION_SUFFIX) else "observed"
        if (row.get("validity") != "fixed-sample-complete"
                or any(field not in row for field in ("bench", "suite", "scenario", "role"))
                or row.get("bench") != shape["bench"] or row.get("observed_blocks") != shape["blocks"]
                or row.get("suite") != shape["suite"] or row.get("scenario") != shape["scenario"]
                or row.get("required_blocks") != shape["blocks"]
                or row.get("role") != role
                or len(row.get("paired_ratios", [])) != shape["blocks"]
                or row.get("order_statistic_ranks") != ranks
                or row.get("status") not in ("pass", "inconclusive", "regression")):
            raise ValueError(f"statistic has missing, duplicate or changed block evidence: {name}")
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
            raise ValueError(f"interval disagrees with paired ratio evidence: {name}")
        expected_status = ("pass" if interval[1] <= 1.10 else
                           "regression" if interval[0] > 1.10 else "inconclusive")
        if row["status"] != expected_status:
            raise ValueError(f"status disagrees with its interval: {name}")
    statuses = {row["status"] for name, row in report["summary"].items() if name.endswith(DECISION_SUFFIX)}
    status = "regression" if "regression" in statuses else (
        "inconclusive" if "inconclusive" in statuses else "pass")
    if report.get("status") != status:
        raise ValueError("aggregate verdict disagrees with the complete decision statistic set")
    return status


def verify_controls(aa: dict, slow: dict) -> dict:
    for report in (aa, slow):
        verify_statistics(report)
    if (aa.get("candidate_control_stretch_percent") != 0 or slow.get("candidate_control_stretch_percent") != 20
            or aa.get("comparison_kind") != "aa-control" or slow.get("comparison_kind") != "slow20-control"):
        raise ValueError("control identities differ from the independent A-A and real-delay design")
    unavailable = {name: {"aa": aa["summary"][name]["status"], "slow20": slow["summary"][name]["status"]}
                   for name in aa["summary"] if name.endswith(DECISION_SUFFIX)
                   and (aa["summary"][name]["status"] != "pass"
                        or slow["summary"][name]["status"] != "regression")}
    return {"status": "measurement-unavailable" if unavailable else "measurement-qualified",
            "unavailable_metrics": unavailable,
            "scope": "A-A noninferiority and +20% artificial wall-clock delay detection on batch-mean decision statistics; batch-P99 statistics are retained as observations and do not qualify or disqualify the measurement; not proof of iid or 10% algorithm sensitivity"}


def protocol_hashes(paths: dict) -> dict:
    return {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in paths.items()}


def loader_overrides(environment: dict) -> list:
    # An external loader path/preload can bypass the adjacent project library
    # identities below. The supported Nightly environment has no such override.
    return sorted(name for name, value in environment.items() if value and
                  (name.startswith("DYLD_") or name in ("LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT")))


def bench_binary(directory: Path, name: str) -> Path:
    choices = [path for path in (directory / name, directory / f"{name}.exe") if path.is_file()]
    if len(choices) != 1:
        raise ValueError(f"expected exactly one benchmark executable for {name} in {directory}; found {len(choices)}")
    return choices[0]


def binary_hashes(directories: dict) -> dict:
    result = {}
    for side, directory in directories.items():
        libraries = {}
        executables = {bench: bench_binary(directory, bench) for bench in BENCHES}
        windows = {path.suffix.lower() == ".exe" for path in executables.values()}
        if len(windows) != 1:
            raise ValueError("mixed native/.exe benchmark outputs in one build directory")
        # Multi-config CMake appends Release (or another configuration) to bin.
        # Never interpret bin/Release's sibling library directory as bin/lib.
        multi_config = directory.parent.name.lower() == "bin"
        root = directory.parent.parent if multi_config else directory.parent
        folders = [directory, root / "lib"]
        if multi_config:
            folders.append(root / "lib" / directory.name)
        # CMake currently places shared libraries in bin; lib also covers an
        # adjacent install-style layout. Keep every alias name and hash through
        # symlinks so retargeting an alias to different bytes changes identity.
        for folder in folders:
            if not folder.is_dir():
                continue
            for path in sorted(folder.iterdir()):
                lower = path.name.lower()
                if lower.endswith((".dylib", ".dll", ".so")) or ".so." in lower:
                    key = path.relative_to(root).as_posix()
                    libraries[key] = hashlib.sha256(path.read_bytes()).hexdigest()
        adjacent_dlls = {}
        if True in windows:
            # The supported Windows build loads both Aria DLLs beside the exe.
            # A DLL found only via PATH/lib is not accepted as an identified
            # project dependency. CRT/system DLLs remain host provenance.
            for component in ("aria_runtime", "aria_abi"):
                matches = [path for path in directory.iterdir()
                           if path.name.lower() in (f"{component}.dll", f"lib{component}.dll")]
                if len(matches) != 1 or not matches[0].is_file():
                    raise ValueError(f"expected one adjacent Windows project DLL for {component}")
                adjacent_dlls[component] = matches[0].relative_to(root).as_posix()
            # .local redirection can bypass the declared adjacent DLL bytes.
            if any(path.name.lower().endswith(".local") for path in directory.iterdir()):
                raise ValueError("Windows .local DLL redirection is unsupported")
        result[side] = {
            "executables": {bench: hashlib.sha256(path.read_bytes()).hexdigest()
                            for bench, path in executables.items()},
            "executable_names": {bench: path.name for bench, path in executables.items()},
            "project_shared_libraries": libraries,
            "windows_adjacent_project_dlls": adjacent_dlls,
        }
    return result


def validate_main(argv=None) -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="strict")
    parser = argparse.ArgumentParser(description="Qualify measurement with independent A-A / +20% delay controls, then compare. Controls unavailable: exit 3, never candidate regression or success. Candidate pass/regression/inconclusive: exit 0/1/3. Invalid setup: exit 2.")
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--slow-control", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--policy", type=Path, default=Path(__file__).resolve().parents[1] / "benchmark/paired-policy.json")
    args = parser.parse_args(argv)
    output = args.output.resolve()
    try:
        output.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        print(f"Invalid evidence directory: {error}", file=sys.stderr)
        return 2
    if any(output.iterdir()):
        print("Evidence directory is not empty; use a fresh directory without overwriting an attempt", file=sys.stderr)
        return 2
    try:
        overrides = loader_overrides(dict(os.environ))
        if overrides:
            raise ValueError("external loader overrides are unsupported for measurement: " + ", ".join(overrides))
        (output / "loader-environment.json").write_text(json.dumps({
            "nonempty_overrides": overrides,
            "checked": "DYLD_*; LD_LIBRARY_PATH; LD_PRELOAD; LD_AUDIT",
            "working_directory": str(Path.cwd()),
            "windows_path": os.environ.get("PATH", "") if os.name == "nt" else None,
            "scope": "project libraries in the executable directory and build-root lib/config directories; Windows requires adjacent Aria runtime/ABI DLLs and rejects .local redirection. PATH/cwd are recorded, not proof of the complete loader search or SxS/CRT/system dependency identity"
        }, indent=2) + "\n", encoding="utf-8")
        directories = {name: path.resolve() for name, path in
                       (("baseline", args.baseline), ("candidate", args.candidate), ("slow-control", args.slow_control))}
        initial_hashes = binary_hashes(directories)
        (output / "binary-sha256.json").write_text(json.dumps(initial_hashes, indent=2) + "\n", encoding="utf-8")
        script = output / "bench-protocol.py"
        policy = output / "paired-policy.json"
        shutil.copyfile(Path(__file__), script)
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
            print(f"Starting {name}: fixed 64 blocks per R scenario and 512 per cheap suite", flush=True)
            with (output / f"{name}.txt").open("wb") as log:
                result = subprocess.run([sys.executable, str(script), "compare", "--baseline", str(directories["baseline"]),
                                         "--candidate", str(candidate), "--output", str(directory),
                                         "--policy", str(policy), "--rounds", "64",
                                         "--expected-candidate-control", str(stretch)], stdout=log, stderr=subprocess.STDOUT)
            phases.append({"phase": name, "exit_code": result.returncode})
            (output / "phase-results.json").write_text(json.dumps(phases, indent=2) + "\n", encoding="utf-8")
            if binary_hashes(directories) != initial_hashes:
                raise ValueError("benchmark executables or project shared libraries changed during measurement")
            if protocol_hashes(frozen_paths) != initial_protocol_hashes:
                raise ValueError("frozen analysis, qualification code or policy changed during measurement")
            if result.returncode not in (0, 1, 3):
                raise RuntimeError(f"{name} failed to measure (exit {result.returncode}); see retained logs")
            report = json.loads((directory / "comparison.json").read_text(encoding="utf-8"))
            status = verify_statistics(report)
            expected_kind = "slow20-control" if stretch else (
                "aa-control" if candidate == directories["baseline"] else "candidate")
            if (report.get("comparison_kind") != expected_kind
                    or report.get("candidate_control_stretch_percent") != stretch
                    or result.returncode != {"pass": 0, "regression": 1, "inconclusive": 3}[status]):
                raise ValueError("phase identity or process exit disagrees with complete statistics")
            return report
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

# ══ profile: calibration profile ═══════════════════════════════════════════

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


def profile_main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Record whether the historical absolute budget's calibration profile applies.")
    parser.add_argument("--build-dir", type=Path, required=True)
    args = parser.parse_args(argv)
    facts = capture(args.build_dir)
    reasons = diagnose(facts)
    print("ARIA_BENCH_PROFILE " + json.dumps({"compatible": not reasons, "reasons": reasons,
                                            "facts": facts}, ensure_ascii=True))
    return 3 if reasons else 0

# ══ scenarios: CLI smoke ═══════════════════════════════════════════════════


def scenarios_main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Smoke-test the real fixed-window CLI; these runs are not performance evidence.")
    parser.add_argument("--bin-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    if args.output.exists() and (not args.output.is_dir() or any(args.output.iterdir())):
        parser.error("output must be a fresh or empty directory; existing evidence is retained")
    args.output.mkdir(parents=True, exist_ok=True)
    result = {"status": "invalid", "scope": "CLI smoke only; not performance qualification"}
    try:
        scenarios = ("list", "filtered", "sorted", "async")
        if set(SCENARIOS) != set(scenarios) or len(EXPECTED) != 4:
            raise ValueError("the smoke check requires all four canonical regression scenarios")
        binary = bench_binary(args.bin_dir.resolve(), "aria_bench_regression")
        result.update({"protocol": PROTOCOL,
                       "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                       "analysis_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                       "valid_invocations": [], "invalid_invocations": []})
        for scenario in (None, *scenarios):
            selector = scenario or "all"
            log = args.output / f"{selector}.txt"
            rows = measure(binary, log, scenario)
            expected = EXPECTED if scenario is None else {
                SCENARIOS[scenario]: EXPECTED[SCENARIOS[scenario]]}
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

# ══ verify-release ═════════════════════════════════════════════════════════


def verify_release(metadata, requested):
    if (metadata.get("tagName") != requested or metadata.get("isDraft") is not False
            or metadata.get("isPrerelease") is not False):
        raise ValueError("benchmark baseline must be the named published stable release")


def verify_release_main(argv=None) -> int:
    verify_release(json.loads(Path(argv[0]).read_text(encoding="utf-8")), argv[1])
    return 0


# ── check: absolute-budget gate ( rewritten from check-bench.sh ) ────────────
CHECK_BENCHES = (
    "aria_bench_iproperty",
    "aria_bench_command",
    "aria_bench_async_command",
    "aria_bench_list",
    "aria_bench_derived_list",
    "aria_bench_trace_sink",
)


def discover_build_dir(repo_root: Path):
    for base in (repo_root / "build/flavors/release", repo_root / "build/flavors/debug",
                 repo_root / "build/flavors/http-no-tls", repo_root / "build"):
        if base.is_dir():
            for layout in ("bin", "benchmark"):
                if (base / layout / "aria_bench_iproperty").exists():
                    return base
            for candidate in base.rglob("aria_bench_*"):
                return base
    return None


def resolve_bench_dir(build_dir: Path):
    override = os.environ.get("ARIA_BENCH_DIR")
    if override:
        return Path(override)
    for candidate in (build_dir / "bin", build_dir / "bin/Release",
                      build_dir / "benchmark", build_dir / "benchmark/Release"):
        if ((candidate / "aria_bench_iproperty").is_file()
                or (candidate / "aria_bench_iproperty.exe").is_file()):
            return candidate
    return build_dir / "bin"


def detect_host_key():
    system = platform.system()
    os_name = {"Darwin": "macos", "Linux": "linux"}.get(
        system, "windows" if system.startswith(("MINGW", "MSYS", "CYGWIN")) else system.lower())
    machine = platform.machine() or ""
    arch = {"x86_64": "x86_64", "amd64": "x86_64", "arm64": "arm64", "aarch64": "aarch64"}.get(
        machine, machine)
    if arch == "aarch64" and os_name == "macos":
        arch = "arm64"
    if arch == "arm64" and os_name == "linux":
        arch = "aarch64"
    return f"{os_name}-{arch}"


def check_benchmark_path(bench_dir: Path, name: str) -> Path:
    native = bench_dir / name
    windows = bench_dir / f"{name}.exe"
    if native.is_file() and windows.is_file() and not native.samefile(windows):
        raise ValueError(f"ambiguous benchmark executables: {native} and {windows}")
    if windows.is_file():
        return windows
    if native.is_file():
        return native
    raise ValueError(f"bench binary not built: {native} or {windows}")


def check_main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Run the benchmark suite and fail if any batch-mean P99 exceeds the ceiling pinned in benchmark/thresholds.json. Exit 0: within budget; 1: regression; 2: setup error; 3: profile unavailable (measurements retained).")
    parser.add_argument("build_dir", type=Path, nargs="?", default=None,
                        help="explicit build dir; auto-discovered when omitted")
    parser.add_argument("--runs", type=int, default=1,
                        help="run the full suite N times and require every run to meet its ceiling")
    args = parser.parse_args(argv)
    repo_root = Path(__file__).resolve().parents[1]
    if args.runs < 1:
        print("error: --runs must be a positive integer", file=sys.stderr)
        return 2
    build_dir = args.build_dir
    if build_dir is None:
        build_dir = discover_build_dir(repo_root)
        if build_dir is None:
            print("error: could not auto-discover a build dir with bench binaries", file=sys.stderr)
            print("tried: build/flavors/{release,debug,http-no-tls}, build/", file=sys.stderr)
            print("hint: pass an explicit path, e.g. scripts/bench.py check build/flavors/release", file=sys.stderr)
            return 2
        print(f"==> auto-discovered build dir: {build_dir}")
    bench_dir = resolve_bench_dir(build_dir)
    thresholds_file = Path(os.environ.get("ARIA_BENCH_THRESHOLDS")
                           or repo_root / "benchmark/thresholds.json")
    host_key = os.environ.get("ARIA_BENCH_HOST") or detect_host_key()
    if not bench_dir.is_dir():
        print(f"error: bench dir not found: {bench_dir}", file=sys.stderr)
        print(f"hint: build the bench targets under {build_dir} first", file=sys.stderr)
        return 2
    if not thresholds_file.is_file():
        print(f"error: thresholds file not found: {thresholds_file}", file=sys.stderr)
        return 2
    profile_lines = []

    def record_profile():
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            status = profile_main(["--build-dir", str(build_dir)])
        captured = buffer.getvalue().rstrip("\n")
        if captured:
            profile_lines.append(captured)
            print(captured, flush=True)
        return status

    status = record_profile()
    if status not in (0, 3):
        return status
    print(f"==> running benchmark suite (host={host_key}, runs={args.runs})", flush=True)
    profile_lines.append(f"==> running benchmark suite (host={host_key}, runs={args.runs})")
    run_lines = []
    for run in range(1, args.runs + 1):
        run_lines.append(f"ARIA_BENCH_RUN {run}")
        if args.runs > 1:
            print(f"  -- run {run}/{args.runs}", flush=True)
        for bench in CHECK_BENCHES:
            try:
                binary = check_benchmark_path(bench_dir, bench)
            except ValueError as error:
                print(f"error: {error}", file=sys.stderr)
                return 2
            print(f"    - {bench}", flush=True)
            try:
                process = subprocess.run([str(binary)], capture_output=True, timeout=600)
            except subprocess.TimeoutExpired:
                print(f"error: {binary.name} timed out", file=sys.stderr)
                return 2
            if process.stderr:
                sys.stderr.write(process.stderr.decode("utf-8", errors="replace"))
            output_text = process.stdout.decode("utf-8", errors="strict")
            print(output_text, end="", flush=True)
            run_lines.extend(output_text.splitlines())
            if process.returncode:
                return process.returncode
    status = record_profile()
    if status not in (0, 3):
        return status
    log_text = "\n".join(profile_lines + run_lines) + "\n"
    return evaluate_thresholds(thresholds_file, log_text, host_key, args.runs)


def evaluate_thresholds(thresholds_path, log_text, host_key, runs):
    import math
    cfg = json.loads(Path(thresholds_path).read_text(encoding="utf-8"))
    hosts = cfg.get("hosts", {}) or {}
    host_block = hosts.get(host_key) or {}
    ceilings = host_block.get("metrics") or cfg.get("metrics") or {}
    ceiling_source = (
        f"hosts.{host_key}" if host_block.get("metrics") else "metrics (generic fallback)")
    if not ceilings:
        print(f"error: no 'metrics' object in {thresholds_path}", file=sys.stderr)
        return 2
    print(f"==> using ceilings from {ceiling_source}")
    for name, ceiling in ceilings.items():
        if (isinstance(ceiling, bool) or not isinstance(ceiling, (int, float))
                or not math.isfinite(ceiling) or ceiling < 0):
            print(f"error: invalid ceiling for {name}: {ceiling}", file=sys.stderr)
            return 2
    line_re = re.compile(
        r"^P\s+(?P<name>.+?)\s+mean=\s*(?P<mean>\S+)ns\s+"
        r"p50=\s*(?P<p50>\S+)ns\s+p95=\s*(?P<p95>\S+)ns\s+"
        r"p99=\s*(?P<p99>\S+)ns\s+\((?P<samples>\d+)x(?P<ops>\d+)\)$")
    worst_p99 = {}
    order = []
    run_metrics = []
    profiles = []
    shapes = {}
    try:
        for raw in log_text.splitlines():
            line = raw.rstrip("\n")
            if line.startswith("ARIA_BENCH_PROFILE "):
                profiles.append(json.loads(line.removeprefix("ARIA_BENCH_PROFILE ")))
                continue
            if line.startswith("ARIA_BENCH_RUN "):
                if int(line.split()[1]) != len(run_metrics) + 1:
                    raise ValueError("unexpected run marker")
                run_metrics.append(set())
                continue
            if not line.startswith("P "):
                continue
            match = line_re.fullmatch(line)
            if not match or not run_metrics:
                raise ValueError(f"malformed percentile row: {line}")
            name = match["name"].strip()
            values = {key: float(match[key]) for key in ("mean", "p50", "p95", "p99")}
            if not all(math.isfinite(v) and v >= 0 for v in values.values()):
                raise ValueError(f"invalid measurement for {name}: {values}")
            if not values["p50"] <= values["p95"] <= values["p99"]:
                raise ValueError(f"unordered percentiles for {name}: {values}")
            if name in run_metrics[-1]:
                raise ValueError(f"duplicate metric in run {len(run_metrics)}: {name}")
            run_metrics[-1].add(name)
            shape = (int(match["samples"]), int(match["ops"]))
            if min(shape) <= 0 or (name in shapes and shapes[name] != shape):
                raise ValueError(f"invalid or changed sample shape for {name}: {shape}")
            shapes[name] = shape
            p99 = values["p99"]
            if name not in worst_p99:
                worst_p99[name] = p99
                order.append(name)
            else:
                worst_p99[name] = max(worst_p99[name], p99)
        if len(run_metrics) != runs:
            raise ValueError(f"expected {runs} runs, found {len(run_metrics)}")
        for index, names in enumerate(run_metrics, 1):
            missing = set(ceilings) - names
            if missing:
                raise ValueError(f"run {index} is missing metrics: {', '.join(sorted(missing))}")
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    profile_required = bool(cfg.get("_absolute_profile"))
    profile_compatible = (not profile_required or (host_key == "macos-arm64" and len(profiles) == 2 and
                          all(profile.get("compatible") is True for profile in profiles)))
    results = [(name, worst_p99[name], ceilings.get(name)) for name in order]
    seen_names = set(order)
    missing_in_log = [k for k in ceilings if k not in seen_names]
    out_targets = [sys.stdout]
    gha = os.environ.get("GITHUB_STEP_SUMMARY")
    if gha:
        out_targets.append(open(gha, "a", encoding="utf-8"))

    def emit(s):
        for t in out_targets:
            t.write(s + "\n")

    emit("")
    emit("### Aria nightly bench — batch-mean P99 ceiling check")
    emit("")
    emit(f"Host: `{host_key}` · ceilings from `{ceiling_source}` · runs: {runs} (worst batch-mean P99 reported)")
    emit("")
    if not profile_compatible:
        emit("**Absolute budget: UNAVAILABLE (exit 3); host/profile is not calibrated.**")
        if host_key != "macos-arm64":
            emit("- Selected ceiling block is not the calibrated macOS ARM64 profile.")
        for profile in profiles:
            for reason in profile.get("reasons", []):
                emit(f"- {reason}")
        emit("Ceiling comparisons below are reference observations, not regression verdicts.")
        emit("")
    emit("| metric | batch-mean p99 (ns) | ceiling (ns) | status |")
    emit("|---|---:|---:|:---:|")
    failures = 0
    unknown = 0
    for name, p99, ceiling in results:
        if ceiling is None:
            emit(f"| {name} | {p99:.1f} | _unknown_ | ⚠ unpinned |")
            unknown += 1
            continue
        ok = p99 <= ceiling
        mark = ("✅" if ok else "❌") if profile_compatible else ("within reference" if ok else "above reference")
        emit(f"| {name} | {p99:.1f} | {ceiling} | {mark} |")
        if not ok:
            failures += 1
    if missing_in_log:
        emit("")
        emit("Threshold keys with no matching bench output:")
        for k in missing_in_log:
            emit(f"- `{k}`")
    emit("")
    if failures or unknown or missing_in_log:
        emit(f"**failures: {failures}, unpinned: {unknown}, missing: {len(missing_in_log)}**")
    elif profile_compatible:
        emit("**all metrics within budget on a compatible profile**")
    for target in out_targets:
        if target is not sys.stdout:
            target.close()
    return 1 if (unknown or missing_in_log) else \
        3 if not profile_compatible else 1 if failures else 0



def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        prog="bench.py",
        description="Aria benchmark toolchain: calibration profile, absolute-budget check, scenario CLI smoke, paired-median comparison, three-phase qualification and baseline release verification.")
    sub = parser.add_subparsers(dest="command", required=True)
    profile_parser = sub.add_parser("profile", help="record whether the historical absolute budget's calibration profile applies")
    profile_parser.add_argument("--build-dir", type=Path, required=True)
    check_parser = sub.add_parser("check", help="run the suite and enforce the pinned P99 ceilings")
    check_parser.add_argument("build_dir", type=Path, nargs="?", default=None)
    check_parser.add_argument("--runs", type=int, default=1)
    scenarios_parser = sub.add_parser("scenarios", help="smoke-test the fixed-window CLI (not performance evidence)")
    scenarios_parser.add_argument("--bin-dir", type=Path, required=True)
    scenarios_parser.add_argument("--output", type=Path, required=True)
    compare_parser = sub.add_parser("compare", help="paired-median fixed-window regression comparison")
    compare_parser.add_argument("--baseline", type=Path, required=True)
    compare_parser.add_argument("--candidate", type=Path, required=True)
    compare_parser.add_argument("--output", type=Path, required=True)
    compare_parser.add_argument("--rounds", type=int, default=64)
    compare_parser.add_argument("--expected-candidate-control", type=int, choices=(0, 20), default=0)
    compare_parser.add_argument("--policy", type=Path, default=Path(__file__).resolve().parents[1] /
                                "benchmark/paired-policy.json")
    validate_parser = sub.add_parser("validate", help="three-phase qualification campaign")
    validate_parser.add_argument("--baseline", type=Path, required=True)
    validate_parser.add_argument("--candidate", type=Path, required=True)
    validate_parser.add_argument("--slow-control", type=Path, required=True)
    validate_parser.add_argument("--output", type=Path, required=True)
    validate_parser.add_argument("--policy", type=Path, default=Path(__file__).resolve().parents[1] /
                                 "benchmark/paired-policy.json")
    verify_parser = sub.add_parser("verify-release", help="reject draft, prerelease and non-release baselines")
    verify_parser.add_argument("metadata", type=Path)
    verify_parser.add_argument("requested")
    args = parser.parse_args(argv)
    if args.command == "profile":
        return profile_main(["--build-dir", str(args.build_dir)])
    if args.command == "check":
        check_argv = []
        if args.build_dir is not None:
            check_argv.append(str(args.build_dir))
        check_argv += ["--runs", str(args.runs)]
        return check_main(check_argv)
    if args.command == "scenarios":
        return scenarios_main(["--bin-dir", str(args.bin_dir), "--output", str(args.output)])
    if args.command == "compare":
        return compare_main(["--baseline", str(args.baseline), "--candidate", str(args.candidate),
                             "--output", str(args.output), "--rounds", str(args.rounds),
                             "--expected-candidate-control", str(args.expected_candidate_control),
                             "--policy", str(args.policy)])
    if args.command == "validate":
        return validate_main(["--baseline", str(args.baseline), "--candidate", str(args.candidate),
                              "--slow-control", str(args.slow_control), "--output", str(args.output),
                              "--policy", str(args.policy)])
    if args.command == "verify-release":
        return verify_release_main([str(args.metadata), args.requested])
    return 2


if __name__ == "__main__":
    sys.exit(main())

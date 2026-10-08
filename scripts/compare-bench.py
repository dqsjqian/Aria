#!/usr/bin/env python3
"""Run alternating same-host baseline/candidate measurements without changing gates."""
from __future__ import annotations

import argparse
import json
import math
import platform
import re
import statistics
import subprocess
from pathlib import Path

BENCHES = ("aria_bench_list", "aria_bench_derived_list", "aria_bench_async_command")
ROW = re.compile(
    r"^P\s+(?P<name>.+?)\s+mean=\s*(?P<mean>\S+)ns\s+"
    r"p50=\s*(?P<p50>\S+)ns\s+p95=\s*(?P<p95>\S+)ns\s+"
    r"p99=\s*(?P<p99>\S+)ns\s+\((?P<samples>\d+)x(?P<ops>\d+)\)$"
)


def measure(binary: Path, log: Path) -> dict:
    try:
        result = subprocess.run([str(binary)], capture_output=True, text=True,
                                encoding="utf-8", timeout=600)
    except subprocess.TimeoutExpired as error:
        def text(value):
            return value.decode("utf-8") if isinstance(value, bytes) else (value or "")
        log.write_text(text(error.stdout) + "\n--- stderr ---\n" + text(error.stderr),
                       encoding="utf-8")
        raise RuntimeError(f"{binary.name} timed out; see {log.name}") from error
    log.write_text(result.stdout + "\n--- stderr ---\n" + result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"{binary.name} exited with {result.returncode}; see {log.name}")
    rows = {}
    for line in result.stdout.splitlines():
        if not line.startswith("P "):
            continue
        match = ROW.fullmatch(line)
        if not match:
            raise ValueError(f"invalid measurement: {line}")
        name = match["name"].strip()
        if name in rows:
            raise ValueError(f"duplicate metric: {name}")
        values = {key: float(match[key]) for key in ("mean", "p50", "p95", "p99")}
        if any(not math.isfinite(value) or value < 0 for value in values.values()):
            raise ValueError(f"non-finite or negative measurement: {name}")
        samples, ops = int(match["samples"]), int(match["ops"])
        if samples <= 0 or ops <= 0:
            raise ValueError(f"invalid sample count: {name}")
        rows[name] = {**values, "samples": samples, "ops": ops}
    if not rows:
        raise ValueError(f"{binary.name} emitted no percentile measurements")
    return rows


def compare(baseline: Path, candidate: Path, output: Path, rounds: int) -> dict:
    output.mkdir(parents=True, exist_ok=True)
    binaries = {"baseline": baseline.resolve(), "candidate": candidate.resolve()}
    expected = {}
    runs = []
    # Warm up both binaries once. Warmup output is retained, never selected as
    # a best-of-N replacement for an actual measurement.
    for bench in BENCHES:
        for side, directory in binaries.items():
            rows = measure(directory / bench, output / f"warmup-{side}-{bench}.txt")
            shape = {name: (row["samples"], row["ops"]) for name, row in rows.items()}
            if bench in expected and expected[bench] != shape:
                raise ValueError(f"baseline/candidate workload mismatch: {bench}")
            expected[bench] = shape
    for index in range(rounds):
        order = ("baseline", "candidate") if index % 2 == 0 else ("candidate", "baseline")
        for bench in BENCHES:
            for side in order:
                rows = measure(binaries[side] / bench, output / f"{index + 1}-{side}-{bench}.txt")
                if {name: (row["samples"], row["ops"]) for name, row in rows.items()} != expected[bench]:
                    raise ValueError(f"measurement workload changed: {bench}")
                runs.append({"round": index + 1, "side": side, "bench": bench, "metrics": rows})
    summary = {}
    for bench, metrics in expected.items():
        for name in metrics:
            series = {side: [run["metrics"][name]["p99"] for run in runs
                             if run["side"] == side and run["bench"] == bench]
                      for side in binaries}
            old, new = (statistics.median(series[side]) for side in binaries)
            summary[name] = {"baseline_p99_ns": series["baseline"],
                             "candidate_p99_ns": series["candidate"],
                             "baseline_median_p99_ns": old, "candidate_median_p99_ns": new,
                             "candidate_over_baseline": new / old if old else None}
    report = {"host": {"system": platform.system(), "machine": platform.machine()},
              "rounds": rounds, "runs": runs, "summary": summary,
              "policy": "Diagnostic paired measurements only; the ordinary ceiling gate is unchanged."}
    (output / "comparison.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=3)
    args = parser.parse_args()
    if not 1 <= args.rounds <= 10:
        parser.error("--rounds must be between 1 and 10")
    report = compare(args.baseline, args.candidate, args.output, args.rounds)
    for name, value in report["summary"].items():
        print(f"{name}: baseline median P99={value['baseline_median_p99_ns']:.1f}ns, "
              f"candidate={value['candidate_median_p99_ns']:.1f}ns")


if __name__ == "__main__":
    main()

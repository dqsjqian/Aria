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


PROTOCOL = "paired-median-scenario-v3"
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
        if (row.get("validity") != "fixed-sample-complete"
                or any(field not in row for field in ("bench", "suite", "scenario"))
                or row.get("bench") != shape["bench"] or row.get("observed_blocks") != shape["blocks"]
                or row.get("suite") != shape["suite"] or row.get("scenario") != shape["scenario"]
                or row.get("required_blocks") != shape["blocks"]
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
    statuses = {row["status"] for row in report["summary"].values()}
    status = "regression" if "regression" in statuses else (
        "inconclusive" if "inconclusive" in statuses else "pass")
    if report.get("status") != status:
        raise ValueError("aggregate verdict disagrees with the complete statistic set")
    return status


def verify_controls(aa: dict, slow: dict) -> dict:
    for report in (aa, slow):
        verify_statistics(report)
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
            print(f"Starting {name}: fixed 64 blocks per R scenario and 512 per cheap suite", flush=True)
            with (output / f"{name}.txt").open("wb") as log:
                result = subprocess.run([sys.executable, str(script), "--baseline", str(directories["baseline"]),
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


if __name__ == "__main__":
    sys.exit(main())

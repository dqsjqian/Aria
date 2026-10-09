"""Offline benchmark gate regressions: retain raw evidence and propagate failures."""
import base64
import importlib.util
import contextlib
import collections
import io
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "check-bench.sh"
BASH = shutil.which("bash")
BENCHES = (
    "aria_bench_iproperty",
    "aria_bench_command",
    "aria_bench_async_command",
    "aria_bench_list",
    "aria_bench_derived_list",
    "aria_bench_trace_sink",
)


@unittest.skipUnless(os.name == "posix" and BASH, "Requires POSIX and bash")
class CheckBenchTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aria-bench-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.bin_dir = self.root / "bin"
        self.bin_dir.mkdir()
        self.thresholds = self.root / "thresholds.json"
        self.thresholds.write_text(json.dumps({"metrics": {"sample metric": 80}}), encoding="utf-8")
        self.summary = self.root / "summary.md"
        for name in BENCHES:
            self.write_bench(name, f"ran {name}\n")
        self.row = "P sample metric mean=10.0ns p50=8.0ns p95=15.0ns p99=20.0ns  (64x50)"
        self.write_bench(BENCHES[0], self.row + "\n", "bench diagnostic\n")

    def write_bench(self, name, output, error="", status=0):
        binary = self.bin_dir / name
        script = (
            "#!/bin/sh\n"
            f"printf '%s' {shlex.quote(output)}\n"
            f"printf '%s' {shlex.quote(error)} >&2\n"
            f"exit {status}\n"
        )
        if sys.platform in ("cygwin", "msys") and (self.bin_dir / f"{name}.exe").is_file():
            # MSYS open() aliases a missing bare name to an existing .exe. Native
            # Windows IO creates the second literal file needed by this fixture.
            windows_path = subprocess.check_output(["cygpath", "-w", str(binary)], text=True).strip()
            powershell = shutil.which("powershell.exe")
            if powershell is None:
                windows_root = (os.environ.get("SYSTEMROOT") or os.environ.get("SystemRoot")
                                or os.environ.get("WINDIR"))
                self.assertTrue(windows_root, "Windows root is required for literal MSYS fixtures")
                posix_root = subprocess.check_output(["cygpath", "-u", windows_root], text=True).strip()
                powershell = str(Path(posix_root) / "System32/WindowsPowerShell/v1.0/powershell.exe")
            encoded_path = base64.b64encode(windows_path.encode("utf-8")).decode("ascii")
            encoded_script = base64.b64encode(script.encode("utf-8")).decode("ascii")
            command = (
                "$ErrorActionPreference = 'Stop'; "
                "[IO.File]::WriteAllBytes("
                f"[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('{encoded_path}')), "
                f"[Convert]::FromBase64String('{encoded_script}'))"
            )
            subprocess.run(
                [powershell, "-NoProfile", "-NonInteractive", "-EncodedCommand",
                 base64.b64encode(command.encode("utf-16le")).decode("ascii")],
                check=True, capture_output=True, timeout=20,
            )
        else:
            binary.write_text(script, encoding="utf-8")
        binary.chmod(0o755)

    def run_bench(self, *args):
        env = dict(os.environ)
        env.update({
            "ARIA_BENCH_DIR": str(self.bin_dir),
            "ARIA_BENCH_THRESHOLDS": str(self.thresholds),
            "ARIA_BENCH_HOST": "test-host",
            "GITHUB_STEP_SUMMARY": str(self.summary),
            "PATH": str(Path(sys.executable).parent) + os.pathsep + env.get("PATH", ""),
        })
        return subprocess.run(
            [BASH, str(SCRIPT), str(self.root), *args],
            env=env, capture_output=True, text=True, encoding="utf-8", timeout=10,
        )

    def test_success_preserves_raw_output_and_separate_summary(self):
        result = self.run_bench()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(self.row, result.stdout)
        self.assertIn("ran " + BENCHES[-1], result.stdout)
        self.assertIn("bench diagnostic", result.stderr)
        summary = self.summary.read_text(encoding="utf-8")
        self.assertIn("| sample metric | 20.0 | 80 |", summary)
        self.assertNotIn(self.row, summary)
        self.assertNotIn("bench diagnostic", summary)

    def test_explicit_exe_names_keep_historical_rows_and_failures(self):
        for name in BENCHES:
            (self.bin_dir / name).rename(self.bin_dir / f"{name}.exe")
        result = self.run_bench()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(self.row, result.stdout)
        bare = self.bin_dir / BENCHES[0]
        windows = self.bin_dir / f"{BENCHES[0]}.exe"
        original = windows.read_bytes()
        self.write_bench(BENCHES[0], "stale competing output\n")
        names = {entry.name for entry in self.bin_dir.iterdir()}
        self.assertTrue({bare.name, windows.name}.issubset(names))
        self.assertFalse(bare.samefile(windows))
        self.assertEqual(windows.read_bytes(), original)
        result = self.run_bench()
        self.assertEqual(result.returncode, 2)
        self.assertIn("ambiguous benchmark", result.stderr)

    def test_regression_keeps_raw_output_and_failure(self):
        row = self.row.replace("p99=20.0ns", "p99=81.0ns")
        self.write_bench(BENCHES[0], row + "\n")
        result = self.run_bench()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(row, result.stdout)
        self.assertIn("failures: 1", result.stdout)

    def test_binary_failure_keeps_partial_output_and_exit_status(self):
        self.write_bench(BENCHES[1], "before failure\n", "binary failed\n", status=7)
        result = self.run_bench()
        self.assertEqual(result.returncode, 7, result.stderr)
        self.assertIn(self.row, result.stdout)
        self.assertIn("before failure", result.stdout)
        self.assertIn("binary failed", result.stderr)
        self.assertNotIn("ran " + BENCHES[-1], result.stdout)
        self.assertFalse(self.summary.exists())

    def test_missing_binary_keeps_completed_output(self):
        (self.bin_dir / BENCHES[1]).unlink()
        result = self.run_bench()
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn(self.row, result.stdout)
        self.assertIn("bench binary not built", result.stderr)

    def test_multiple_runs_preserve_each_raw_measurement(self):
        result = self.run_bench("--runs", "2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count(self.row), 2)
        self.assertIn("runs: 2 (worst batch-mean P99 reported)", result.stdout)

    def test_invalid_measurement_is_visible_and_rejected(self):
        row = self.row.replace("p99=20.0ns", "p99=nanns")
        self.write_bench(BENCHES[0], row + "\n")
        result = self.run_bench()
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn(row, result.stdout)
        self.assertIn("invalid measurement", result.stderr)

    def test_stderr_is_not_parsed_as_an_extra_measurement(self):
        self.write_bench(BENCHES[0], self.row + "\n", self.row + "\n")
        result = self.run_bench()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count(self.row), 1)
        self.assertEqual(result.stderr.count(self.row), 1)

    def test_second_good_run_cannot_erase_first_failure(self):
        counter = self.root / "invoked"
        binary = self.bin_dir / BENCHES[0]
        bad = self.row.replace("p99=20.0ns", "p99=100.0ns")
        binary.write_text("#!/bin/sh\n" +
                          f"if [ -f {shlex.quote(str(counter))} ]; then\n" +
                          f"printf '%s\\n' {shlex.quote(self.row)}\nelse\n" +
                          f"touch {shlex.quote(str(counter))}\n" +
                          f"printf '%s\\n' {shlex.quote(bad)}\nfi\n", encoding="utf-8")
        binary.chmod(0o755)
        result = self.run_bench("--runs", "2")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("| sample metric | 100.0 | 80 |", result.stdout)

    def test_uncalibrated_profile_is_explicit_and_nonzero(self):
        config = {"_absolute_profile": "required", "metrics": {"sample metric": 80}}
        self.thresholds.write_text(json.dumps(config), encoding="utf-8")
        result = self.run_bench()
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertIn("Absolute budget: UNAVAILABLE", result.stdout)
        self.assertNotIn("all metrics within budget", result.stdout)


class PairedBenchTests(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location("compare_bench", SCRIPT.with_name("compare-bench.py"))
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)
        temporary = tempfile.TemporaryDirectory(prefix="aria-paired-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        for side in ("old", "new", "slow"):
            directory = self.root / side
            directory.mkdir()
            for bench in self.module.BENCHES:
                (directory / bench).write_bytes(b"mock collection fixture; never executed")
        self.row = "R metric mean=10ns p50=8ns p95=15ns p99=20ns  (64x50)\n"
        self.valid = "C ARIA_BENCH_CONTROL stretch_percent=0\n" + self.module.WORKLOAD + "\nARIA_SCENARIO all\n" + self.row
        self.original_expected = dict(self.module.EXPECTED)
        patch = mock.patch.object(self.module, "EXPECTED", {"metric": (64, 50)})
        patch.start()
        self.addCleanup(patch.stop)

    def test_executable_resolution_requires_one_existing_native_or_exe_file(self):
        directory = self.root / "old"
        name = self.module.BENCHES[0]
        native = directory / name
        windows = directory / f"{name}.exe"
        native.rename(windows)
        self.assertEqual(self.module.bench_binary(directory, name), windows)
        native.write_bytes(b"ambiguous stale output")
        with self.assertRaisesRegex(ValueError, "exactly one"):
            self.module.bench_binary(directory, name)
        native.unlink()
        windows.unlink()
        with self.assertRaisesRegex(ValueError, "exactly one"):
            self.module.bench_binary(directory, name)

    def test_cli_main_hashes_exe_files_and_propagates_all_verdicts(self):
        for side in ("old", "new"):
            for bench in self.module.BENCHES:
                (self.root / side / bench).rename(self.root / side / f"{bench}.exe")
        for status, code in (("pass", 0), ("regression", 1), ("inconclusive", 3)):
            output = self.root / f"cli-{status}"
            summary = {f"{name} / batch-{statistic}": {
                "status": status, "ratio_interval": [0.9, 1.05], "baseline_repeat_ratios": [1.0]}
                for bench in self.module.BENCHES
                for name in self.module.LEGACY.get(bench, self.original_expected)
                for statistic in ("mean", "p99")}
            report = {"status": status, "comparison_kind": "candidate", "summary": summary}
            args = ["compare-bench.py", "--baseline", str(self.root / "old"),
                    "--candidate", str(self.root / "new"), "--output", str(output)]
            with self.subTest(status=status), mock.patch.object(sys, "argv", args), \
                 mock.patch.object(self.module, "compare", return_value=report) as compare, \
                 mock.patch.dict(os.environ, {}, clear=True), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(self.module.main(), code)
                compare.assert_called_once()
            identities = json.loads((output / "binary-sha256.json").read_text())
            self.assertEqual(set(identities["candidate"]), set(self.module.BENCHES))
            self.assertFalse((output / "error.json").exists())

    def test_raw_samples_are_retained_and_must_match_reported_percentiles(self):
        row = "R metric mean=10ns p50=10ns p95=10ns p99=10ns  (64x50)\n"
        raw = {"metric": "metric", "batch_means_ns": [10] * 64}
        output = "C ARIA_BENCH_CONTROL stretch_percent=0\n" + self.module.WORKLOAD + "\nARIA_SCENARIO all\n" + row + "S " + json.dumps(raw) + "\n"
        result = subprocess.CompletedProcess([], 0, output.encode(), b"")
        with mock.patch.object(self.module.subprocess, "run", return_value=result):
            measured = self.module.measure(Path(self.module.BENCHES[0]), self.root / "raw.txt")
        self.assertEqual(measured["metric"]["batch_means_ns"], raw["batch_means_ns"])
        for invalid in (output.replace("p99=10ns", "p99=20ns"), output.split("S ")[0],
                        output + "S " + json.dumps(raw) + "\n"):
            result = subprocess.CompletedProcess([], 0, invalid.encode(), b"")
            with mock.patch.object(self.module.subprocess, "run", return_value=result):
                with self.assertRaises(ValueError):
                    self.module.measure(Path(self.module.BENCHES[0]), self.root / "bad-raw.txt")

    def test_measure_retains_output_on_binary_failure(self):
        result = subprocess.CompletedProcess([], 7, b"partial\n", b"failed\xff\n")
        log = self.root / "failed.txt"
        with mock.patch.object(self.module.subprocess, "run", return_value=result):
            with self.assertRaisesRegex(RuntimeError, "exited with 7"):
                self.module.measure(Path(self.module.BENCHES[0]), log)
        self.assertIn(b"partial", log.read_bytes())
        self.assertIn(b"failed\xff", log.read_bytes())

    def test_selected_scenario_has_exact_marker_rows_raw_and_cli(self):
        def output(scenario):
            shape = self.original_expected if scenario == "all" else {
                self.module.SCENARIOS[scenario]: self.original_expected[self.module.SCENARIOS[scenario]]}
            return ("C ARIA_BENCH_CONTROL stretch_percent=20\n" + self.module.WORKLOAD +
                    f"\nARIA_SCENARIO {scenario}\n" + "".join(
                        f"R {name} mean=12ns p50=12ns p95=12ns p99=12ns  ({samples}x{ops})\n" +
                        "S " + json.dumps({"metric": name, "batch_means_ns": [12] * samples}) + "\n"
                        for name, (samples, ops) in shape.items()))
        binary = Path("aria_bench_regression.exe")
        with mock.patch.object(self.module, "EXPECTED", self.original_expected):
            for scenario in (None, *self.module.SCENARIOS):
                text = output(scenario or "all")
                result = subprocess.CompletedProcess([], 0, text.encode(), b"")
                with self.subTest(scenario=scenario), mock.patch.object(
                        self.module.subprocess, "run", return_value=result) as runner:
                    rows = self.module.measure(binary, self.root / "scenario.txt", scenario=scenario)
                    runner.assert_called_once_with([str(binary)] + (
                        ["--scenario", scenario] if scenario is not None else []),
                        capture_output=True, timeout=600)
                    self.assertEqual(set(rows), set(self.original_expected) if scenario is None else {
                        self.module.SCENARIOS[scenario]})
                    self.assertTrue(all(row["control_stretch_percent"] == 20 for row in rows.values()))
            valid = output("list")
            name = self.module.SCENARIOS["list"]
            extra = f"R unexpected mean=12ns p50=12ns p95=12ns p99=12ns  (1024x200)\n"
            invalids = (output("all"), output("filtered"), valid.replace("ARIA_SCENARIO list\n", ""),
                        valid + "ARIA_SCENARIO list\n", valid.replace("ARIA_SCENARIO list", "ARIA_SCENARIO all"),
                        valid + extra, valid + extra.replace("R ", "P "),
                        valid.replace('"metric": "' + name + '"', '"metric": "wrong"'),
                        valid.replace("(1024x200)", "(1024x201)"), valid.replace("p99=12ns", "p99=13ns"))
            for text in invalids:
                result = subprocess.CompletedProcess([], 0, text.encode(), b"")
                with self.subTest(text=text[:120]), mock.patch.object(
                        self.module.subprocess, "run", return_value=result), self.assertRaises(ValueError):
                    self.module.measure(binary, self.root / "wrong-scenario.txt", scenario="list")
            result = subprocess.CompletedProcess([], 0, valid.encode(), b"")
            with mock.patch.object(self.module.subprocess, "run", return_value=result), self.assertRaises(ValueError):
                self.module.measure(binary, self.root / "wrong-default.txt")
            with mock.patch.object(self.module.subprocess, "run") as runner:
                for scenario in ("unknown", "all", ""):
                    with self.subTest(scenario=scenario), self.assertRaises(ValueError):
                        self.module.measure(binary, self.root / "unknown.txt", scenario=scenario)
                with self.assertRaises(ValueError):
                    self.module.measure(Path("aria_bench_command.exe"), self.root / "wrong-binary.txt", scenario="list")
                runner.assert_not_called()

    def test_timeout_preserves_partial_measurements(self):
        error = subprocess.TimeoutExpired("bench", 600, output=b"partial\n", stderr=b"diagnostic\n")
        log = self.root / "timeout.txt"
        with mock.patch.object(self.module.subprocess, "run", side_effect=error):
            with self.assertRaisesRegex(RuntimeError, "timed out"):
                self.module.measure(Path(self.module.BENCHES[0]), log)
        self.assertIn("partial", log.read_text(encoding="utf-8"))
        self.assertIn("diagnostic", log.read_text(encoding="utf-8"))

    def test_measure_rejects_invalid_or_changed_workloads(self):
        for output in ("nothing\n", self.row, self.valid + self.row,
                       self.valid.replace("p99=20ns", "p99=nanns"),
                       self.valid.replace("p99=20ns", "p99=2ns"),
                       self.valid.replace("(64x50)", "(0x50)"),
                       self.valid.replace("metric", "unknown"),
                       self.valid.replace("fixed-window-v2", "fixed-window-v3")):
            with self.subTest(output=output):
                result = subprocess.CompletedProcess([], 0, output.encode(), b"")
                with mock.patch.object(self.module.subprocess, "run", return_value=result):
                    with self.assertRaises(ValueError):
                        self.module.measure(Path(self.module.BENCHES[0]), self.root / "invalid.txt")

    def test_all_legacy_gate_metrics_remain_covered(self):
        config = json.loads(SCRIPT.parents[1].joinpath("benchmark/thresholds.json").read_text(encoding="utf-8"))
        historical = set(config["metrics"])
        replaced = {"ObservableList::push_back (no observers)",
                    "FilteredList: source push_back (n=10k)",
                    "SortedList: source push_back random (n=10k)",
                    "AsyncCommand<int,int>::execute round-trip"}
        legacy = {name for metrics in self.module.LEGACY.values() for name in metrics}
        self.assertEqual(legacy, historical - replaced)
        for bench, shape in self.module.LEGACY.items():
            output = "C ARIA_BENCH_CONTROL stretch_percent=0\n" + "".join(
                f"P {name} mean=10ns p50=10ns p95=10ns p99=10ns  ({samples}x{ops})\n" +
                "S " + json.dumps({"metric": name, "batch_means_ns": [10] * samples}) + "\n"
                for name, (samples, ops) in shape.items())
            result = subprocess.CompletedProcess([], 0, output.encode(), b"")
            for suffix in ("", ".exe", ".EXE"):
                with self.subTest(bench=bench, suffix=suffix), \
                     mock.patch.object(self.module.subprocess, "run", return_value=result):
                    self.assertEqual(set(self.module.measure(Path(bench + suffix), self.root / "legacy.txt")), set(shape))

    def test_paired_runs_balance_order_and_keep_every_measurement(self):
        calls, saved = [], {}
        for side in ("old", "new"):
            for bench in self.module.BENCHES:
                (self.root / side / bench).rename(self.root / side / f"{bench}.exe")
        class Checkpoint(io.StringIO):
            def close(self):
                pass
        checkpoint = Checkpoint()
        warmup_checkpoint = Checkpoint()
        original_open = Path.open
        def open_checkpoint(path, *args, **kwargs):
            if path.name == "warmups.jsonl":
                return warmup_checkpoint
            if path.name == "measurements.jsonl":
                return checkpoint
            return original_open(path, *args, **kwargs)
        def measure(binary, log, scenario=None):
            calls.append((binary.name, binary.parent.name))
            value = 20.0 if binary.parent.name == "old" else 10.0
            self.assertEqual(binary.suffix, ".exe")
            shape = self.module.LEGACY[binary.stem] if scenario is None else {self.module.SCENARIOS[scenario]: self.original_expected[self.module.SCENARIOS[scenario]]}
            rows = {name: {"mean": value, "p50": value, "p95": value, "p99": value,
                           "samples": samples, "ops": ops, "control_stretch_percent": 0}
                    for name, (samples, ops) in shape.items()}
            saved[log.name] = "retained fixture output"
            return rows
        def save(path, content, **kwargs):
            saved[path.name] = content
            return len(content)
        with mock.patch.object(self.module, "EXPECTED", self.original_expected), \
             mock.patch.object(self.module, "measure", side_effect=measure), \
             mock.patch.object(Path, "write_text", autospec=True, side_effect=save), \
             mock.patch.object(Path, "open", autospec=True, side_effect=open_checkpoint):
            report = self.module.compare(self.root / "old", self.root / "new", self.root / "out", 64)
        self.assertEqual(len(calls), 7182)  # 14 retained warmups + 7168 measured process runs
        self.assertEqual(len(report["runs"]), 7168)
        self.assertEqual(len(report["warmups"]), 14)
        self.assertEqual(collections.Counter((row["suite"], row["side"]) for row in report["warmups"]),
                         {(suite, side): 1 for suite in self.module.SUITES for side in ("baseline", "candidate")})
        self.assertEqual(len(report["summary"]), 24)
        self.assertEqual(len(json.loads(saved["schedule.json"])), 1792)
        warmup_events = [json.loads(line) for line in warmup_checkpoint.getvalue().splitlines()]
        self.assertEqual(collections.Counter(row["event"] for row in warmup_events), {"started": 14, "completed": 14})
        events = [json.loads(line) for line in checkpoint.getvalue().splitlines()]
        self.assertEqual(collections.Counter(row["event"] for row in events), {"started": 7168, "completed": 7168})
        for suite, blocks in self.module.BLOCK_PLAN.items():
            measured = [run for run in report["runs"] if run["suite"] == suite]
            self.assertEqual(len(measured), blocks * 4)
            orientations = [[run["side"] for run in measured if run["block"] == i]
                            for i in range(1, blocks + 1)]
            self.assertEqual(orientations.count(["baseline", "candidate", "candidate", "baseline"]), blocks // 2)
            self.assertEqual(orientations.count(["candidate", "baseline", "baseline", "candidate"]), blocks // 2)
        for run in report["runs"]:
            self.assertIn(run["raw_log"], saved)
            self.assertEqual(run["provenance"]["phase_executables"]["file"], "binary-sha256.json")
            self.assertEqual(len(run["provenance"]["analysis_sha256"]), 64)
            self.assertNotIn(":", run["raw_log"])
            self.assertEqual({key: run[key] for key in ("bench", "scenario")}, self.module.SUITES[run["suite"]])
            self.assertLessEqual(run["monotonic_start_ns"], run["monotonic_end_ns"])
        self.assertEqual(len({row["raw_log"] for row in report["runs"]}), 7168)
        for row in report["summary"].values():
            self.assertTrue(all(abs(ratio - 0.5) < 1e-12 for ratio in row["paired_ratios"]))
        self.assertEqual(report["status"], "pass")
        self.assertAlmostEqual(report["joint_coverage_lower_bound_if_iid"], 0.9606807140106389)
        self.assertEqual(json.loads(saved["progress.json"])["completed_blocks_by_suite"], self.module.BLOCK_PLAN)
        self.assertIn("comparison.json", saved)

    def test_collection_failure_keeps_started_and_failed_checkpoint(self):
        campaign = {"binary-sha256.json": "frozen project library identities",
                    "protocol-sha256.json": "frozen analysis, qualification and policy identities"}
        for name, content in campaign.items():
            (self.root / name).write_text(content)
        completed = 0
        def measure(binary, log, scenario=None):
            nonlocal completed
            completed += 1
            if completed > 14:
                log.write_bytes(b"partial failed output")
                raise RuntimeError("fixture failure")
            return {"metric": {"mean": 1, "p99": 1, "control_stretch_percent": 0}}
        with mock.patch.object(self.module, "measure", side_effect=measure):
            with self.assertRaisesRegex(RuntimeError, "fixture failure"):
                self.module.compare(self.root / "old", self.root / "new", self.root / "out", 64)
        events = [json.loads(line) for line in (self.root / "out/measurements.jsonl").read_text().splitlines()]
        self.assertEqual([row["event"] for row in events], ["started", "failed"])
        self.assertEqual(events[0]["provenance"], events[1]["provenance"])
        for key, filename in (("campaign_binaries_and_libraries", "binary-sha256.json"),
                              ("campaign_protocol", "protocol-sha256.json")):
            identity = events[0]["provenance"][key]
            self.assertEqual(identity["file"], "../" + filename)
            self.assertEqual(identity["sha256"], self.module.hashlib.sha256(campaign[filename].encode()).hexdigest())
        self.assertEqual((self.root / "out" / events[0]["raw_log"]).read_bytes(), b"partial failed output")
        self.assertFalse((self.root / "out/comparison.json").exists())

    def test_warmup_failure_keeps_started_failed_and_raw_without_timed_sampling(self):
        def fail(binary, log, scenario=None):
            log.write_bytes(b"warmup partial output")
            raise RuntimeError("fixture warmup failure")
        with mock.patch.object(self.module, "measure", side_effect=fail):
            with self.assertRaisesRegex(RuntimeError, "warmup failure"):
                self.module.compare(self.root / "old", self.root / "new", self.root / "warmup-out", 64)
        output = self.root / "warmup-out"
        events = [json.loads(line) for line in (output / "warmups.jsonl").read_text().splitlines()]
        self.assertEqual([row["event"] for row in events], ["started", "failed"])
        self.assertEqual(events[0]["suite"], "aria_bench_regression--list")
        self.assertEqual(events[0]["scenario"], "list")
        self.assertEqual((output / events[0]["raw_log"]).read_bytes(), b"warmup partial output")
        self.assertFalse((output / "measurements.jsonl").exists())
        self.assertFalse((output / "comparison.json").exists())

    def test_wrong_delay_control_binary_is_rejected_before_measurement(self):
        row = {"metric": {"mean": 10, "p50": 10, "p95": 10, "p99": 10,
                          "samples": 64, "ops": 50, "control_stretch_percent": 0}}
        with mock.patch.object(self.module, "measure", return_value=row):
            with self.assertRaisesRegex(ValueError, "unexpected measurement control"):
                self.module.compare(self.root / "old", self.root / "slow", self.root / "out", 64,
                                    expected_candidate_control=20)

    def test_exact_order_statistic_ranks_and_joint_coverage(self):
        low, high, coverage = self.module.median_interval_ranks(64)
        self.assertEqual((low, high), (20, 45))
        self.assertAlmostEqual(coverage, 0.9624930557241274)
        with self.assertRaises(ValueError):
            self.module.median_interval_ranks(4)

    def test_median_estimand_preserves_extremes_without_selecting_best_runs(self):
        blocks = [{"baseline": [100, 100], "candidate": [100, 100]}] * 63 + [
            {"baseline": [100, 100], "candidate": [200, 200]}]
        row = self.module.classify(blocks, 1.10)
        self.assertEqual(row["status"], "pass")
        self.assertEqual(row["worst_case_envelope"], [1, 2])
        self.assertEqual(len(row["paired_ratios"]), 64)
        self.assertAlmostEqual(max(row["paired_ratios"]), 2)

    def test_noise_overlap_wrong_count_and_known_slowdown(self):
        cases = [([{"baseline": [10, 10], "candidate": [20, 20]}] * 64, "regression"),
                 ([{"baseline": [10, 12], "candidate": [1, 1]}] * 64, "pass"),
                 ([{"baseline": [10, 10], "candidate": [1, 1]}] * 63, "inconclusive"),
                 ([{"baseline": [10, 10], "candidate": [1, 1]}] * 65, "inconclusive"),
                 ([{"baseline": [10, 10], "candidate": [10, 10]}] * 32 +
                  [{"baseline": [10, 10], "candidate": [12, 12]}] * 32, "inconclusive")]
        for blocks, expected in cases:
            with self.subTest(expected=expected):
                self.assertEqual(self.module.classify(blocks, 1.10)["status"], expected)

    def test_rounding_uncertainty_can_only_widen_the_interval(self):
        plain = [{"baseline": [1, 1], "candidate": [1.09, 1.09]}] * 64
        uncertain = [dict(block, rounding_radius_ns=0.05) for block in plain]
        before, after = (self.module.classify(blocks, 1.10) for blocks in (plain, uncertain))
        self.assertEqual(before["status"], "pass")
        self.assertEqual(after["status"], "inconclusive")
        self.assertLess(after["ratio_interval"][0], before["ratio_interval"][0])
        self.assertGreater(after["ratio_interval"][1], before["ratio_interval"][1])


class HistoricalProfileTests(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location("bench_profile", SCRIPT.with_name("bench-profile.py"))
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)
        self.facts = {"system": "Darwin", "machine": "arm64", "cpu": "Apple M3 Pro",
                      "kernel": "RELEASE_ARM64", "compiler": "Apple clang version 21.0.0",
                      "cpu_count": 12, "load": [1, 2, 3], "cache": {"CMAKE_BUILD_TYPE": "Release"}}
        self.facts["cache"]["ARIA_BENCH_CONTROL_STRETCH_PERCENT"] = "0"
        self.facts["cache"].update({"CMAKE_CXX_FLAGS": "", "CMAKE_CXX_FLAGS_RELEASE": "-O3 -DNDEBUG"})
        for kind in ("EXE", "SHARED", "MODULE"):
            self.facts["cache"][f"CMAKE_{kind}_LINKER_FLAGS"] = ""
            self.facts["cache"][f"CMAKE_{kind}_LINKER_FLAGS_RELEASE"] = ""
        self.facts["cache"].update({f"ARIA_ENABLE_{name}": "OFF" for name in
                                   ("ASAN", "UBSAN", "TSAN", "MSAN")})

    def test_quiet_calibration_host_is_compatible(self):
        self.assertEqual(self.module.diagnose(self.facts), [])

    def test_injected_sanitizer_or_optimization_flags_are_uncalibrated(self):
        for key, value in (("CMAKE_CXX_FLAGS", "-fsanitize=address"),
                           ("CMAKE_EXE_LINKER_FLAGS", "-fsanitize=thread"),
                           ("CMAKE_CXX_FLAGS_RELEASE", "-O0 -DNDEBUG")):
            with self.subTest(key=key):
                cache = dict(self.facts["cache"], **{key: value})
                self.assertTrue(self.module.diagnose(dict(self.facts, cache=cache)))

    def test_vm_load_compiler_and_sanitizer_cannot_claim_profile(self):
        for key, value in (("kernel", "RELEASE_ARM64_VMAPPLE"), ("load", [8, 2, 3]),
                           ("compiler", "clang version 21"), ("cpu", "Apple M4 Pro")):
            with self.subTest(key=key):
                self.assertTrue(self.module.diagnose(dict(self.facts, **{key: value})))
        self.facts["cache"]["ARIA_ENABLE_ASAN"] = "ON"
        self.assertTrue(self.module.diagnose(self.facts))


if __name__ == "__main__":
    unittest.main()

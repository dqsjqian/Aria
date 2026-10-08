"""Offline benchmark gate regressions: retain raw evidence and propagate failures."""
import importlib.util
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
        binary.write_text(
            "#!/bin/sh\n"
            f"printf '%s' {shlex.quote(output)}\n"
            f"printf '%s' {shlex.quote(error)} >&2\n"
            f"exit {status}\n",
            encoding="utf-8",
        )
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
        self.assertIn("runs: 2 (best P99 reported)", result.stdout)

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


class PairedBenchTests(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location("compare_bench", SCRIPT.with_name("compare-bench.py"))
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)
        temporary = tempfile.TemporaryDirectory(prefix="aria-paired-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.row = "P metric mean=10ns p50=8ns p95=15ns p99=20ns  (64x50)\n"

    def test_measure_retains_output_on_binary_failure(self):
        result = subprocess.CompletedProcess([], 7, "partial\n", "failed\n")
        log = self.root / "failed.txt"
        with mock.patch.object(self.module.subprocess, "run", return_value=result):
            with self.assertRaisesRegex(RuntimeError, "exited with 7"):
                self.module.measure(Path("bench"), log)
        self.assertIn("partial", log.read_text(encoding="utf-8"))
        self.assertIn("failed", log.read_text(encoding="utf-8"))

    def test_timeout_preserves_partial_measurements(self):
        error = subprocess.TimeoutExpired("bench", 600, output=b"partial\n", stderr=b"diagnostic\n")
        log = self.root / "timeout.txt"
        with mock.patch.object(self.module.subprocess, "run", side_effect=error):
            with self.assertRaisesRegex(RuntimeError, "timed out"):
                self.module.measure(Path("bench"), log)
        self.assertIn("partial", log.read_text(encoding="utf-8"))
        self.assertIn("diagnostic", log.read_text(encoding="utf-8"))

    def test_measure_rejects_missing_duplicate_and_nonfinite_rows(self):
        for output in ("nothing\n", self.row * 2, self.row.replace("p99=20ns", "p99=nanns"),
                       self.row.replace("(64x50)", "(0x50)")):
            with self.subTest(output=output):
                result = subprocess.CompletedProcess([], 0, output, "")
                with mock.patch.object(self.module.subprocess, "run", return_value=result):
                    with self.assertRaises(ValueError):
                        self.module.measure(Path("bench"), self.root / "invalid.txt")

    def test_paired_runs_alternate_and_keep_every_measurement(self):
        calls = []

        def measure(binary, log):
            calls.append(binary.parent.name)
            value = 20.0 if binary.parent.name == "old" else 10.0
            return {"metric": {"mean": value, "p50": value, "p95": value, "p99": value,
                               "samples": 64, "ops": 50}}

        with mock.patch.object(self.module, "measure", side_effect=measure), \
             mock.patch.object(self.module, "BENCHES", ("bench",)):
            report = self.module.compare(self.root / "old", self.root / "new", self.root / "out", 3)
        self.assertEqual(calls, ["old", "new", "old", "new", "new", "old", "old", "new"])
        self.assertEqual(len(report["runs"]), 6)
        self.assertEqual(report["summary"]["metric"]["baseline_p99_ns"], [20, 20, 20])
        self.assertEqual(report["summary"]["metric"]["candidate_over_baseline"], 0.5)
        self.assertTrue((self.root / "out/comparison.json").is_file())

    def test_workload_mismatch_is_rejected(self):
        first = {"metric": {"samples": 64, "ops": 50}}
        second = {"metric": {"samples": 64, "ops": 40}}
        with mock.patch.object(self.module, "measure", side_effect=[first, second]):
            with self.assertRaisesRegex(ValueError, "workload mismatch"):
                self.module.compare(self.root / "old", self.root / "new", self.root / "out", 1)


if __name__ == "__main__":
    unittest.main()

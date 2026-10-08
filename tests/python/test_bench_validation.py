"""Qualify complete controls before measuring a candidate; retain failed attempts."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/run-bench-validation.py"
spec = importlib.util.spec_from_file_location("bench_validation", SCRIPT)
validation = importlib.util.module_from_spec(spec)
spec.loader.exec_module(validation)


def report(status, control):
    summary = {}
    for name, shape in validation.EXPECTED_STATISTICS.items():
        n = shape["blocks"]
        values = ([1.0] * (n // 2) + [1.2] * (n // 2) if status == "inconclusive"
                  else [1.0 if status == "pass" else 1.2] * n)
        summary[name] = {"bench": shape["bench"], "validity": "fixed-sample-complete",
                         "observed_blocks": n, "required_blocks": n,
                         "paired_ratios": values,
                         "paired_ratio_rounding_intervals": [[v, v] for v in values],
                         "ratio_interval": [min(values), max(values)],
                         "order_statistic_ranks": [20, 45] if n == 64 else [221, 292],
                         "status": status}
    return {"macro_blocks": 64, "blocks_by_bench": validation.BLOCK_PLAN,
            "workload": "ARIA_WORKLOAD fixed-window-v2 batch-mean-nearest-rank",
            "policy": {"protocol": "paired-median-stratified-v2", "family_alpha": .05,
                       "family_comparisons": 24, "ratio_limit": 1.1,
                       "blocks_by_bench": validation.BLOCK_PLAN},
            "comparison_kind": "slow20-control" if control else "aa-control",
            "candidate_control_stretch_percent": control, "summary": summary, "status": status}


class MeasurementQualificationTests(unittest.TestCase):
    def test_every_statistic_must_qualify_in_both_independent_controls(self):
        result = validation.verify_controls(report("pass", 0), report("regression", 20))
        self.assertEqual(result["status"], "measurement-qualified")

    def test_unknown_or_false_control_results_never_qualify(self):
        for status in ("inconclusive", "regression"):
            aa = report("pass", 0)
            name = next(iter(aa["summary"]))
            aa["summary"][name] = report(status, 0)["summary"][name]
            result = validation.verify_controls(aa, report("regression", 20))
            self.assertEqual(result["status"], "measurement-unavailable")
            self.assertEqual(set(result["unavailable_metrics"]), {name})
        for status in ("pass", "inconclusive"):
            self.assertEqual(validation.verify_controls(report("pass", 0), report(status, 20))["status"],
                             "measurement-unavailable")

    def test_protocol_identity_exact_names_and_complete_counts_are_required(self):
        for key, value in (("macro_blocks", 63), ("summary", {}), ("blocks_by_bench", {}),
                           ("candidate_control_stretch_percent", 0), ("workload", "other")):
            slow = report("regression", 20)
            slow[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                validation.verify_controls(report("pass", 0), slow)
        slow = report("regression", 20)
        name = next(iter(slow["summary"]))
        slow["summary"]["fake metric"] = slow["summary"].pop(name)
        with self.assertRaises(ValueError):
            validation.verify_controls(report("pass", 0), slow)
        for field, value in (("observed_blocks", 511), ("required_blocks", 64),
                             ("paired_ratios", [1.2] * 511), ("order_statistic_ranks", [20, 45])):
            slow = report("regression", 20)
            name = next(name for name, row in slow["summary"].items() if row["observed_blocks"] == 512)
            slow["summary"][name][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                validation.verify_controls(report("pass", 0), slow)

    def test_nonfinite_values_forged_interval_and_false_verdict_are_rejected(self):
        for field, value in (("paired_ratios", [1.2] * 64), ("paired_ratios", [float("nan")] * 64),
                             ("ratio_interval", [1.2, 1.2]), ("status", "regression"),
                             ("validity", "wrong-sample-count")):
            aa = report("pass", 0)
            name = next(name for name, row in aa["summary"].items() if row["observed_blocks"] == 64)
            aa["summary"][name][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                validation.verify_controls(aa, report("regression", 20))


class ValidationRunnerTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="aria-validation-test-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.directories = {side: self.root / side / "bin" for side in ("baseline", "candidate", "slow")}
        for directory in self.directories.values():
            directory.mkdir(parents=True)
            for bench in validation.BENCHES:
                (directory / bench).write_bytes(b"fixture identity; never executed")
        self.runtime = self.directories["baseline"] / "libaria_runtime.3.dylib"
        self.runtime.write_bytes(b"fixture original runtime")
        self.output = self.root / "evidence"
        self.calls = []

    def run_runner(self, aa_status="pass", corruption=None, process_failure=False):
        def phase(command, **kwargs):
            directory = Path(command[command.index("--output") + 1])
            name = directory.name
            self.calls.append(name)
            if process_failure:
                return subprocess.CompletedProcess(command, 2)
            status = aa_status if name == "aa-control" else "regression" if name == "slow20-control" else "pass"
            value = report(status, 20 if name == "slow20-control" else 0)
            directory.mkdir(parents=True)
            (directory / "comparison.json").write_text(json.dumps(value))
            if corruption == "policy":
                (self.output / "paired-policy.json").write_text("changed frozen policy")
            if corruption == "binary":
                (self.directories["baseline"] / validation.BENCHES[0]).write_bytes(b"changed binary")
            if corruption == "library-content":
                self.runtime.write_bytes(b"changed runtime library")
            if corruption == "library-deleted":
                self.runtime.unlink()
            if corruption == "library-added":
                self.runtime.with_name("added.dll").write_bytes(b"unexpected library")
            if corruption in ("alias-retarget", "alias-dangling"):
                self.runtime.unlink()
                target = "alternate_runtime.dylib" if corruption == "alias-retarget" else "absent_runtime.dylib"
                self.runtime.symlink_to(target)
            return subprocess.CompletedProcess(command, {"pass": 0, "regression": 1, "inconclusive": 3}[status])
        args = [str(SCRIPT), "--baseline", str(self.directories["baseline"]),
                "--candidate", str(self.directories["candidate"]), "--slow-control", str(self.directories["slow"]),
                "--policy", str(ROOT / "benchmark/paired-policy.json"), "--output", str(self.output)]
        with mock.patch.object(sys, "argv", args), mock.patch.object(validation.subprocess, "run", side_effect=phase), \
             mock.patch.dict(validation.os.environ, {}, clear=True), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return validation.main()

    def test_qualified_controls_precede_independent_candidate(self):
        self.assertEqual(self.run_runner(), 0)
        self.assertEqual(self.calls, ["aa-control", "slow20-control", "candidate"])
        result = json.loads((self.output / "validation.json").read_text())
        self.assertEqual(result["status"], "pass")
        self.assertEqual(set(json.loads((self.output / "protocol-sha256.json").read_text())),
                         {"analysis", "policy", "qualification"})

    def test_unavailable_control_finishes_both_controls_but_never_measures_candidate(self):
        self.assertEqual(self.run_runner(aa_status="inconclusive"), 3)
        self.assertEqual(self.calls, ["aa-control", "slow20-control"])
        result = json.loads((self.output / "validation.json").read_text())
        self.assertEqual(result["candidate_status"], "not-measured")
        self.assertEqual(result["status"], "measurement-unavailable")

    def test_measurement_process_failure_is_invalid_not_regression(self):
        self.assertEqual(self.run_runner(process_failure=True), 2)
        self.assertEqual(self.calls, ["aa-control"])
        self.assertEqual(json.loads((self.output / "validation-error.json").read_text())["status"], "invalid")

    def test_frozen_policy_or_binary_change_cannot_accept_a_result(self):
        for corruption in ("policy", "binary"):
            self.output = self.root / corruption
            self.calls.clear()
            with self.subTest(corruption=corruption):
                self.assertEqual(self.run_runner(corruption=corruption), 2)
                self.assertEqual(self.calls, ["aa-control"])
                self.assertIn("changed", json.loads((self.output / "validation-error.json").read_text())["error"])

    def test_library_addition_removal_or_replacement_invalidates_measurement(self):
        for corruption in ("library-content", "library-deleted", "library-added"):
            self.output = self.root / corruption
            self.runtime.write_bytes(b"fixture original runtime")
            self.calls.clear()
            with self.subTest(corruption=corruption):
                self.assertEqual(self.run_runner(corruption=corruption), 2)
                self.assertEqual(self.calls, ["aa-control"])
                self.assertIn("shared libraries changed", json.loads(
                    (self.output / "validation-error.json").read_text())["error"])

    def test_library_suffixes_and_adjacent_lib_are_covered_with_portable_keys(self):
        binary_dir = self.directories["baseline"]
        lib = binary_dir.parent / "lib"
        lib.mkdir()
        for name in ("libsample.so", "libsample.so.3.2", "sample.DLL", "libsample.dylib"):
            (lib / name).write_bytes(name.encode())
        identities = validation.binary_hashes(self.directories)["baseline"]["project_shared_libraries"]
        self.assertEqual(set(identities), {"bin/libaria_runtime.3.dylib", "lib/libsample.so",
                                          "lib/libsample.so.3.2", "lib/sample.DLL", "lib/libsample.dylib"})

    def test_library_alias_retarget_or_dangling_target_is_invalid(self):
        original = self.runtime.with_name("original_runtime.dylib")
        alternate = self.runtime.with_name("alternate_runtime.dylib")
        original.write_bytes(b"original runtime")
        alternate.write_bytes(b"alternate runtime")
        for corruption in ("alias-retarget", "alias-dangling"):
            self.runtime.unlink(missing_ok=True)
            try:
                self.runtime.symlink_to(original.name)
            except (OSError, NotImplementedError) as error:
                self.skipTest(f"This host does not permit symbolic links: {error}")
            initial = validation.binary_hashes(self.directories)["baseline"]["project_shared_libraries"]
            self.assertEqual(initial["bin/libaria_runtime.3.dylib"], initial["bin/original_runtime.dylib"])
            self.output = self.root / corruption
            self.calls.clear()
            with self.subTest(corruption=corruption):
                self.assertEqual(self.run_runner(corruption=corruption), 2)
                self.assertEqual(self.calls, ["aa-control"])
                self.assertEqual(json.loads((self.output / "validation-error.json").read_text())["status"], "invalid")

    def test_external_loader_override_is_rejected_before_subprocess(self):
        self.assertEqual(validation.loader_overrides({"DYLD_LIBRARY_PATH": "", "LD_PRELOAD": ""}), [])
        for name in ("DYLD_LIBRARY_PATH", "DYLD_INSERT_LIBRARIES", "LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT"):
            self.assertEqual(validation.loader_overrides({name: "external"}), [name])
        args = [str(SCRIPT), "--baseline", str(self.directories["baseline"]),
                "--candidate", str(self.directories["candidate"]), "--slow-control", str(self.directories["slow"]),
                "--output", str(self.output)]
        with mock.patch.object(sys, "argv", args), mock.patch.object(validation.subprocess, "run") as runner, \
             mock.patch.dict(validation.os.environ, {"DYLD_LIBRARY_PATH": "external"}, clear=True), \
             contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(validation.main(), 2)
            runner.assert_not_called()

    def test_partial_existing_evidence_is_never_modified(self):
        self.output.mkdir()
        marker = self.output / "protocol-sha256.json"
        marker.write_text("original evidence")
        self.assertEqual(self.run_runner(), 2)
        self.assertEqual(self.calls, [])
        self.assertEqual(list(self.output.iterdir()), [marker])
        self.assertEqual(marker.read_text(), "original evidence")


if __name__ == "__main__":
    unittest.main()

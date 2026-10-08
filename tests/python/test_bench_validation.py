"""Measurement qualification must not label an unusable detector a regression."""
import importlib.util
from pathlib import Path
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "scripts/run-bench-validation.py"
spec = importlib.util.spec_from_file_location("bench_validation", SCRIPT)
validation = importlib.util.module_from_spec(spec)
spec.loader.exec_module(validation)


def report(status, control):
    return {"rounds": 64, "policy": {"protocol": "paired-median-v1"},
            "workload": "ARIA_WORKLOAD fixed-window-v2 batch-mean-nearest-rank",
            "candidate_control_stretch_percent": control,
            "summary": {f"metric-{i}": {"status": status} for i in range(24)}}


class MeasurementQualificationTests(unittest.TestCase):
    def test_every_statistic_must_qualify_in_both_independent_controls(self):
        result = validation.verify_controls(report("pass", 0), report("regression", 20))
        self.assertEqual(result["status"], "measurement-qualified")

    def test_aa_noise_or_false_regression_is_measurement_unavailable(self):
        for status in ("inconclusive", "regression"):
            aa = report("pass", 0)
            aa["summary"]["metric-17"]["status"] = status
            result = validation.verify_controls(aa, report("regression", 20))
            self.assertEqual(result["status"], "measurement-unavailable")
            self.assertEqual(set(result["unavailable_metrics"]), {"metric-17"})

    def test_missing_delay_detection_cannot_be_replaced_by_other_passing_metrics(self):
        for status in ("pass", "inconclusive"):
            slow = report("regression", 20)
            slow["summary"]["metric-23"]["status"] = status
            result = validation.verify_controls(report("pass", 0), slow)
            self.assertEqual(result["status"], "measurement-unavailable")

    def test_changed_protocol_missing_metrics_or_fake_control_is_rejected(self):
        for key, value in (("rounds", 63), ("summary", {}),
                           ("candidate_control_stretch_percent", 0), ("workload", "other")):
            with self.subTest(key=key):
                slow = report("regression", 20)
                slow[key] = value
                with self.assertRaises(ValueError):
                    validation.verify_controls(report("pass", 0), slow)


if __name__ == "__main__":
    unittest.main()

"""Offline stratified analysis contracts; never executes a benchmark."""
import collections
import copy
import importlib.util
from pathlib import Path
import unittest
import tempfile
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[2] / 'scripts'

def load(name, file):
    spec = importlib.util.spec_from_file_location(name, ROOT / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module
compare = load('stratified_compare', 'compare-bench.py')
validation = load('stratified_validation', 'run-bench-validation.py')

def report(status, control):
    value = 1.0 if status == "pass" else 1.2
    interval = [1.0, 1.2] if status == "inconclusive" else [value, value]
    summary = {}
    for name, shape in compare.expected_statistics().items():
        count = shape["blocks"]
        values = ([1.0] * (count // 2) + [1.2] * (count // 2)
                  if status == "inconclusive" else [value] * count)
        summary[name] = {
            **shape, "validity": "fixed-sample-complete",
            "observed_blocks": count, "required_blocks": count,
            "paired_ratios": values,
            "paired_ratio_rounding_intervals": [[item, item] for item in values],
            "ratio_interval": interval,
            "order_statistic_ranks": [20, 45] if count == 64 else [221, 292],
            "status": status,
        }
    return {
        "macro_blocks": 64, "blocks_by_bench": compare.BLOCK_PLAN,
        "workload": compare.WORKLOAD,
        "policy": {"protocol": compare.PROTOCOL, "family_alpha": 0.05,
                   "family_comparisons": 24, "ratio_limit": 1.1,
                   "blocks_by_bench": compare.BLOCK_PLAN},
        "comparison_kind": "slow20-control" if control else "aa-control",
        "candidate_control_stretch_percent": control, "summary": summary,
    }


class StratifiedProtocolTests(unittest.TestCase):

    def test_schedule_fixed_complete_individually_balanced_and_interleaved(self):
        schedule = compare.make_schedule()
        self.assertEqual(schedule, compare.make_schedule())
        self.assertEqual(collections.Counter((x['bench'] for x in schedule)), compare.BLOCK_PLAN)
        for bench, n in compare.BLOCK_PLAN.items():
            rows = [x for x in schedule if x['bench'] == bench]
            self.assertEqual([x['block'] for x in rows], list(range(1, n + 1)))
            self.assertEqual(collections.Counter((x['order'][0] for x in rows)), {'baseline': n // 2, 'candidate': n // 2})
            self.assertTrue(all((collections.Counter(x['order']) == {'baseline': 2, 'candidate': 2} for x in rows)))
        for macro in range(1, 65):
            self.assertEqual(collections.Counter((x['bench'] for x in schedule if x['macro'] == macro)), dict.fromkeys(compare.BENCHES[1:], 8) | {compare.BENCHES[0]: 1})
        self.assertGreater(len({x['macro_slot'] for x in schedule if x['family'] == 'heavy'}), 1)

    def test_exact_ranks_and_mixed_coverage(self):
        self.assertEqual(compare.median_interval_ranks(64)[:2], (20, 45))
        self.assertEqual(compare.median_interval_ranks(256)[:2], (103, 154))
        self.assertEqual(compare.median_interval_ranks(512)[:2], (221, 292))
        failures = [(1 - compare.median_interval_ranks(x['blocks'])[2]) / 24 for x in compare.expected_statistics().values()]
        self.assertAlmostEqual(1 - sum(failures), 0.9606807140106389)

    def test_fixed_counts_no_optional_extension_or_missing_block(self):
        block = {'baseline': [10, 10], 'candidate': [10, 10]}
        for n in (511, 512, 513):
            row = compare.classify([block] * n, 1.1, required_blocks=512)
            self.assertEqual(row['status'], 'pass' if n == 512 else 'inconclusive')

    def test_candidate_regression_uncertainty_and_all_samples_retained(self):
        good = {'baseline': [10, 10], 'candidate': [10, 10]}
        bad = {'baseline': [10, 10], 'candidate': [12, 12]}
        for blocks, status in (([good] * 512, 'pass'), ([bad] * 512, 'regression'), ([good] * 256 + [bad] * 256, 'inconclusive')):
            row = compare.classify(blocks, 1.1, required_blocks=512)
            self.assertEqual(row['status'], status)
            self.assertEqual(len(row['paired_ratios']), 512)

    def test_control_signature_matches_comparator(self):
        self.assertEqual(validation.EXPECTED_STATISTICS, compare.expected_statistics())
        self.assertEqual(validation.verify_controls(report('pass', 0), report('regression', 20))['status'], 'measurement-qualified')

    def test_qualification_rejects_changed_or_missing_evidence(self):
        original = report('regression', 20)
        key = next((k for k, v in original['summary'].items() if v['blocks'] == 512))
        for field, value in (('observed_blocks', 511), ('required_blocks', 64), ('paired_ratios', [1.2] * 511), ('order_statistic_ranks', [20, 45]), ('bench', 'other')):
            slow = copy.deepcopy(original)
            slow['summary'][key][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                validation.verify_controls(report('pass', 0), slow)
        slow = copy.deepcopy(original)
        slow['summary']['fake'] = slow['summary'].pop(key)
        with self.assertRaises(ValueError):
            validation.verify_controls(report('pass', 0), slow)

    def test_unknown_or_false_control_results_never_qualify(self):
        for status in ('inconclusive', 'regression'):
            self.assertEqual(validation.verify_controls(report(status, 0), report('regression', 20))['status'], 'measurement-unavailable')
        self.assertEqual(validation.verify_controls(report('pass', 0), report('pass', 20))['status'], 'measurement-unavailable')

    def test_qualification_rejects_nonfinite_or_inconsistent_verdict(self):
        for field, value in (('paired_ratios', [1.2] * 64), ('paired_ratios', [float('nan')] * 64), ('ratio_interval', [1.2, 1.2]), ('status', 'regression'), ('validity', 'wrong-sample-count')):
            aa = report('pass', 0)
            name = next((k for k, v in aa['summary'].items() if v['blocks'] == 64))
            aa['summary'][name][field] = value
            with self.subTest(field=field, value=str(value)[:30]), self.assertRaises(ValueError):
                validation.verify_controls(aa, report('regression', 20))

    def test_partial_attempt_directory_is_rejected_without_any_write(self):
        for script in ('compare-bench.py', 'run-bench-validation.py'):
            with tempfile.TemporaryDirectory() as temp:
                out = Path(temp)
                (out / 'protocol-sha256.json').write_text('original evidence')
                args = [sys.executable, str(ROOT / script), '--baseline', '/missing', '--candidate', '/missing', '--output', str(out), '--policy', str(ROOT.parent / 'benchmark/paired-policy.json')]
                if script == 'run-bench-validation.py':
                    args += ['--slow-control', '/missing']
                result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                self.assertEqual(result.returncode, 2)
                self.assertEqual([p.name for p in out.iterdir()], ['protocol-sha256.json'])
                self.assertEqual((out / 'protocol-sha256.json').read_text(), 'original evidence')
if __name__ == '__main__':
    unittest.main()

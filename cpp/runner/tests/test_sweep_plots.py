import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import sweep_plot_common as common
import plot_sweep
import plot_sweep_v3
import plot_compare


class SweepTests(unittest.TestCase):
    def tearDown(self):
        common.load_styles(None)

    def test_missing_and_overflow_are_not_zero(self):
        data = {"latency_ns": {"all": {"mean": 123, "p95": 150, "p99": None}}}
        common.enrich_metrics(data)
        self.assertEqual(data["latency_all_mean_ns"], 123)
        self.assertTrue(math.isnan(data["latency_all_p99_ns"]))
        self.assertTrue(math.isnan(data["jain_fairness"]))
        self.assertTrue(math.isnan(plot_sweep_v3.aggregate([150, float("nan")], "mean")[0]))

    def test_trimmed_zeros_and_unknown_count(self):
        data = {"worker_count": 16, "sum_num_operations_by_thread": [100] * 4}
        common.enrich_metrics(data)
        self.assertEqual(data["zero_progress_percent"], 75)
        self.assertEqual(data["jain_fairness"], 0.25)
        self.assertEqual(len(data["sum_num_operations_by_thread"]), 16)
        old = {"sum_num_operations_by_thread": [100] * 4}
        common.enrich_metrics(old)
        self.assertTrue(math.isnan(old["zero_progress_percent"]))

    def test_repeat_fairness_is_not_computed_from_pooled_work(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for run, counts in (("v1", [100, 0]), ("v2", [0, 100])):
                ds = root / run / "stack"
                ds.mkdir(parents=True)
                (ds / "result_cops2.json").write_text(json.dumps({
                    "worker_count": 2, "sum_num_operations_by_thread": counts,
                    "latency_ns": {"all": {"mean": 100 if run == "v1" else 300, "p95": 500, "p99": None}}
                }))
            targets, multi, _ = plot_sweep_v3.discover_targets(root, set(), ["auto"], "mean")
            compared = plot_compare.load_setup(root, set(), "mean")
            self.assertEqual(compared["stack"][0]["jain_fairness"], 0.5)
            self.assertEqual(compared["stack"][0]["latency_all_mean_ns"], 200)
            self.assertTrue(math.isnan(compared["stack"][0]["latency_all_p99_ns"]))
            record = targets["stack"][0]
            self.assertTrue(multi)
            self.assertEqual(record["jain_fairness"], 0.5)
            self.assertEqual(record["zero_progress_percent"], 50)
            self.assertEqual(record["latency_all_mean_ns"], 200)
            self.assertTrue(math.isnan(record["latency_all_p99_ns"]))

    def test_style_and_axis_labels_reach_plot(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            style = root / "styles.json"
            style.write_text(json.dumps({"stack": {"label": "Stack A", "color": "red", "marker": None}}))
            common.load_styles(style)
            data = {"coroutines": 16, "zero_progress_percent": 75}
            with patch.object(common.plt, "close"):
                common.plot_extra_metrics({"stack": [data]}, root, {"zero_progress_percent"}, ["all"])
                ax = common.plt.gcf().axes[0]
                self.assertEqual(ax.lines[0].get_label(), "Stack A")
                self.assertEqual(ax.lines[0].get_color(), "red")
                self.assertEqual(ax.lines[0].get_marker(), "")
                self.assertEqual(ax.get_xlabel(), common.AXIS_LABELS["x"])
            common.plt.close("all")
            with patch.object(common.plt, "close"):
                plot_compare.plot_comparison(
                    [("yield", {"stack": [data]}), ("no-yield", {"stack": [data]})],
                    "zero_progress_percent", "test Y", "test", root, "mean", "compare.png")
                ax = common.plt.gcf().axes[0]
                self.assertEqual([line.get_color() for line in ax.lines], ["red", "red"])
                self.assertEqual([line.get_linestyle() for line in ax.lines], ["-", "--"])
                self.assertEqual(ax.get_xlabel(), common.AXIS_LABELS["compare_x"])
                self.assertEqual(ax.artists[0].get_texts()[0].get_text(), "Stack A")
            common.plt.close("all")
            style.write_text('{"stack": {"color": "invalid-color"}}')
            with self.assertRaises(ValueError):
                common.load_styles(style)

    def test_all_clis_generate_old_and_new_plots(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for run in ("v1", "v2"):
                for ds in ("stack", "other"):
                    folder = root / run / ds
                    folder.mkdir(parents=True)
                    for cops in (4, 16):
                        (folder / f"result_cops{cops}.json").write_text(json.dumps({
                            "worker_count": cops, "sum_num_operations_by_thread": [100] * 4,
                            "sum_num_operations_total": 400, "max_time_thread_terminate_total": 1000000,
                            "latency_ns": {"all": {"mean": 50, "p95": 75, "p99": 100},
                                           "pop": {"mean": 40, "p95": 65, "p99": 90}}
                        }))
            styles = root / "styles.json"
            styles.write_text('{"stack": {"label": "My stack", "color": "red", "marker": null}}')
            compare_out = root / "compare-out"
            compare = subprocess.run([
                sys.executable, str(Path(plot_compare.__file__)),
                "--run", f"single {root / 'v1'}", "--run", f"repeated {root}",
                "--output-dir", str(compare_out), "--ds-styles", str(styles),
                "--latency-operations", "all", "pop"
            ], capture_output=True, text=True, timeout=60)
            self.assertEqual(compare.returncode, 0, compare.stderr)
            for name in ("throughput", "combined", "latency_all_p99_ns", "latency_pop_mean_ns",
                         "zero_progress_percent", "jain_fairness"):
                self.assertTrue((compare_out / f"compare_{name}.png").is_file(), name)
            for script in ("plot_sweep.py", "plot_sweep_v3.py"):
                output = root / (script + "-out")
                cmd = [sys.executable, str(Path(__file__).resolve().parents[1] / script),
                       "--results-dir", str(root if "v3" in script else root / "v1"),
                       "--output-dir", str(output), "--ds-styles", str(styles),
                       "--latency-operations", "all", "pop"]
                if "v3" in script:
                    cmd += ["--repeats", "auto"]
                completed = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
                self.assertEqual(completed.returncode, 0, completed.stderr)
                for name in ("throughput", "latency_all_mean_ns", "latency_pop_p99_ns", "zero_progress_percent", "jain_fairness"):
                    self.assertTrue((output / f"{name}_vs_coroutines.png").is_file(), name)
                self.assertTrue((output / "per_thread_distribution.png").is_file())


if __name__ == "__main__":
    unittest.main()

"""Smoke and determinism tests for the analysis sweep (SYNTHETIC data, tiny grid)."""
import importlib.util
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("sweep", ROOT / "python" / "analysis" / "sweep.py")
sweep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sweep)

ARGS = ["--seeds", "3", "--messages", "3000", "--gammas", "0.05", "0.5", "--latencies-us", "0", "1000"]


def test_sweep_outputs_and_determinism(tmp_path):
    a, b = tmp_path / "a", tmp_path / "b"
    sweep.main(ARGS + ["--workers", "1", "--out", str(a)])
    sweep.main(ARGS + ["--workers", "3", "--out", str(b)])
    # the raw per-run numbers do not depend on the number of threads
    assert (a / "sweep_runs.csv").read_text() == (b / "sweep_runs.csv").read_text()
    rows = (a / "sweep_runs.csv").read_text().strip().splitlines()
    assert len(rows) == 1 + 3 * 2 * 2
    report = (a / "sweep_report.md").read_text()
    for needle in ("SYNTHETIC", "Fill-model artifact rate", "constant latency", "not evidence of profitability",
                   "k is not calibrated", "Hidden", "sigma"):
        assert needle.lower() in report.lower(), needle
    assert report.count("Fill-model artifact rate for this table") == 5  # beside every result table
    assert len(list((a / "plots").glob("*.png"))) == 4
    assert "wall time" in (a / "environment.txt").read_text()


def test_needs_two_seeds(tmp_path):
    with pytest.raises(SystemExit):
        sweep.main(["--seeds", "1", "--out", str(tmp_path)])

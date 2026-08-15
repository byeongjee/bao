"""Unit tests for scripts/otii/preprocess_traces.py (needs `--extra plot`)."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest

np = pytest.importorskip("numpy")

pytestmark = pytest.mark.unit

_SCRIPT = Path(__file__).parent.parent / "scripts" / "otii" / "preprocess_traces.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("preprocess_traces", _SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import {_SCRIPT}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


dt = _load_module()


def _read_csv(path):
    rows = np.loadtxt(path, delimiter=",", skiprows=1)
    return rows[:, 0], rows[:, 1]


class TestBlockAverage:
    def test_drops_incomplete_tail(self):
        out = dt.block_average(np.array([1.0, 3.0, 9.0]), 2)
        assert list(out) == [2.0]


class TestMain:
    @pytest.fixture
    def raw_trace(self, tmp_path):
        # 40 samples at 1 kHz -> 2 samples at 50 Hz.
        path = tmp_path / "7.txt"
        rows = [f"{i}\t{1.0 if i < 20 else 2.0}" for i in range(40)]
        path.write_text("\n".join(rows) + "\n")
        return path

    def _run(self, raw_trace, out_dir, extra):
        argv = [
            "preprocess_traces.py",
            str(raw_trace),
            "-o",
            str(out_dir),
            *extra,
        ]
        old = sys.argv
        sys.argv = argv
        try:
            dt.main()
        finally:
            sys.argv = old
        return out_dir / f"{raw_trace.stem}.csv"

    def test_repeat_tiles_and_extends_time(self, raw_trace, tmp_path):
        out = self._run(raw_trace, tmp_path / "out", ["--repeat", "3"])
        times, volts = _read_csv(out)
        assert list(volts) == [1.0, 2.0] * 3
        assert list(times) == pytest.approx([0.0, 0.02, 0.04, 0.06, 0.08, 0.10])

    def test_scaled_trace_is_scaled_before_clipping(
        self, raw_trace, tmp_path, monkeypatch
    ):
        monkeypatch.setitem(dt.SCALE_TO_MAX, raw_trace.stem, 3.6)
        out = self._run(raw_trace, tmp_path / "out", ["--vmax", "3.6"])
        _, volts = _read_csv(out)
        # Peak 2.0 scaled to 3.6, so the 1.0 sample follows to 1.8; a clip
        # applied first would have left both untouched.
        assert list(volts) == pytest.approx([1.8, 3.6])

    def test_rejects_zero_repeat(self, raw_trace, tmp_path):
        with pytest.raises(SystemExit):
            self._run(raw_trace, tmp_path / "out", ["--repeat", "0"])

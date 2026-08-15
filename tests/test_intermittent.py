"""Unit tests for the intermittent-power replay helpers — pure functions."""

from __future__ import annotations

import pytest
from ckpt.device.otii import _PulseDetector
from ckpt.errors import ConfigError
from ckpt.intermittent.runner import load_trace

pytestmark = pytest.mark.unit


def _edges(*pairs):
    """Build GPI event dicts from (timestamp, value) pairs."""
    return [{"timestamp": ts, "value": val} for ts, val in pairs]


class TestPulseDetector:
    def test_start_then_stop(self):
        d = _PulseDetector()
        # 10 us start pulse, then 5 ms stop pulse.
        d.feed(_edges((1.0, True), (1.00001, False), (2.0, True), (2.005, False)))
        assert d.start_fall_ts == 1.00001
        assert d.stop_rise_ts == 2.0

    def test_no_stop_pulse(self):
        d = _PulseDetector()
        d.feed(_edges((1.0, True), (1.00001, False)))
        assert d.stop_rise_ts is None
        assert d.start_fall_ts == 1.00001

    def test_multiple_start_pulses_keeps_first(self):
        # A run that dies before its first checkpoint boots fresh again and
        # emits another start pulse; timing spans from the first one.
        d = _PulseDetector()
        d.feed(_edges((1.0, True), (1.00001, False), (3.0, True), (3.00001, False)))
        d.feed(_edges((5.0, True), (5.005, False)))
        assert d.start_fall_ts == 1.00001
        assert d.stop_rise_ts == 5.0

    def test_incremental_feed_across_pulse(self):
        d = _PulseDetector()
        d.feed(_edges((1.0, True), (1.00001, False), (2.0, True)))
        assert d.stop_rise_ts is None
        d.feed(_edges((2.005, False)))
        assert d.stop_rise_ts == 2.0

    def test_events_after_stop_ignored(self):
        d = _PulseDetector()
        d.feed(_edges((2.0, True), (2.005, False), (9.0, True), (9.005, False)))
        assert d.stop_rise_ts == 2.0


class TestLoadTrace:
    def test_loads_samples(self, tmp_path):
        p = tmp_path / "t.csv"
        p.write_text("time_s,voltage_v\n0.0,0.5\n0.02,2.5\n")
        assert load_trace(p) == [(0.0, 0.5), (0.02, 2.5)]

    def test_rejects_wrong_header(self, tmp_path):
        p = tmp_path / "t.csv"
        p.write_text("t,v\n0.0,0.5\n")
        with pytest.raises(ConfigError):
            load_trace(p)

    def test_rejects_empty(self, tmp_path):
        p = tmp_path / "t.csv"
        p.write_text("time_s,voltage_v\n")
        with pytest.raises(ConfigError):
            load_trace(p)

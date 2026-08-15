"""Unit tests for the intermittent-power replay helpers — pure functions."""

from __future__ import annotations

import pytest
from ckpt.device.otii import _PulseDetector
from ckpt.env import ProjectEnv
from ckpt.errors import ConfigError
from ckpt.intermittent.runner import load_trace, resolve_traces

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

    def test_classification_around_1ms_threshold(self):
        # A stretched 1.2 ms high window counts as a stop-pulse candidate
        # (e.g. a marginal-VCC boot letting the start pulse decay slowly);
        # the runner cross-checks it against __nvm_done before trusting it.
        d = _PulseDetector()
        d.feed(_edges((1.0, True), (1.0012, False)))
        assert d.stop_rise_ts == 1.0

        d = _PulseDetector()
        d.feed(_edges((1.0, True), (1.0009, False)))
        assert d.stop_rise_ts is None
        assert d.start_fall_ts == 1.0009


class TestResolveTraces:
    @pytest.fixture
    def env(self, tmp_path):
        trace_dir = tmp_path / "benchmarks" / "traces"
        trace_dir.mkdir(parents=True)
        (trace_dir / "1.csv").write_text("time_s,voltage_v\n0.0,1.0\n")
        return ProjectEnv.from_environ(project_dir=tmp_path)

    def test_resolves_by_name(self, env, tmp_path):
        assert resolve_traces(env, ["1"]) == [
            tmp_path / "benchmarks" / "traces" / "1.csv"
        ]

    def test_resolves_by_name_with_suffix(self, env, tmp_path):
        assert resolve_traces(env, ["1.csv"]) == [
            tmp_path / "benchmarks" / "traces" / "1.csv"
        ]

    def test_resolves_by_path(self, env, tmp_path):
        p = tmp_path / "elsewhere.csv"
        p.write_text("time_s,voltage_v\n0.0,1.0\n")
        assert resolve_traces(env, [str(p)]) == [p]

    def test_missing_raises(self, env):
        with pytest.raises(ConfigError):
            resolve_traces(env, ["nope"])


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

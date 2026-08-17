"""Unit tests for the intermittent-power replay helpers — pure functions."""

from __future__ import annotations

import threading

import pytest
from ckpt.device.otii import OtiiSession, replay_trace
from ckpt.device.saleae import _collect_pulses, _replay_timing_from_pulses
from ckpt.env import ProjectEnv
from ckpt.errors import ConfigError
from ckpt.intermittent.runner import load_trace, resolve_traces

pytestmark = pytest.mark.unit


class TestReplayTimingFromPulses:
    def test_start_then_stop(self):
        # 10 us start pulse, then 5 ms stop pulse.
        us = _replay_timing_from_pulses([(1.0, 1.00001), (2.0, 2.005)])
        assert us == pytest.approx((2.0 - 1.00001) * 1e6)

    def test_multiple_start_pulses_uses_first(self):
        # A run that dies before its first checkpoint boots fresh and emits
        # another start pulse; timing spans from the first attempt.
        us = _replay_timing_from_pulses([(1.0, 1.00001), (3.0, 3.00001), (5.0, 5.005)])
        assert us == pytest.approx((5.0 - 1.00001) * 1e6)

    def test_power_up_glitch_is_not_a_start_pulse(self):
        # Each cold power-up glitches P3.4 for ~0.4 us before the real pulse.
        us = _replay_timing_from_pulses(
            [(0.9, 0.90000038), (1.0, 1.00001), (2.0, 2.005)]
        )
        assert us == pytest.approx((2.0 - 1.00001) * 1e6)

    def test_no_stop_pulse(self):
        assert _replay_timing_from_pulses([(1.0, 1.00001)]) is None

    def test_no_start_pulse(self):
        assert _replay_timing_from_pulses([(2.0, 2.005)]) is None

    def test_start_pulses_after_stop_ignored(self):
        us = _replay_timing_from_pulses([(1.0, 1.00001), (2.0, 2.005), (9.0, 9.00001)])
        assert us == pytest.approx((2.0 - 1.00001) * 1e6)


class TestCollectPulses:
    def test_pairs_edges(self, tmp_path):
        p = tmp_path / "digital.csv"
        p.write_text("Time [s],Channel 0\n0.0,0\n1.0,1\n1.00001,0\n2.0,1\n2.005,0\n")
        assert _collect_pulses(p) == [(1.0, 1.00001), (2.0, 2.005)]

    def test_trailing_high_ignored(self, tmp_path):
        p = tmp_path / "digital.csv"
        p.write_text("Time [s],Channel 0\n0.0,0\n1.0,1\n")
        assert _collect_pulses(p) == []


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


class FakeArc:
    """Records the main-output calls replay_trace makes."""

    def __init__(self, stop_event, stop_after_calls):
        self.stop_event = stop_event
        self.stop_after_calls = stop_after_calls
        self.voltages = []
        self.main = []

    def set_main_voltage(self, voltage):
        self.voltages.append(voltage)
        if len(self.voltages) == self.stop_after_calls:
            self.stop_event.set()

    def set_main(self, on):
        self.main.append(on)


class TestReplayTrace:
    def test_plays_whole_trace(self):
        event = threading.Event()
        arc = FakeArc(event, stop_after_calls=None)
        samples = [(0.0, 1.0), (0.02, 2.0), (0.04, 3.0)]
        replay_trace(OtiiSession(otii=None, arc=arc), samples, event)
        assert arc.voltages == [1.0, 1.0, 2.0, 3.0]
        assert arc.main == [True, False]

    def test_stops_early_when_event_set(self):
        event = threading.Event()
        # Fires on the third call: the pre-loop voltage plus two samples.
        arc = FakeArc(event, stop_after_calls=3)
        samples = [(i * 0.02, 1.0 + i) for i in range(200)]  # 4 s if played whole
        seconds = replay_trace(OtiiSession(otii=None, arc=arc), samples, event)
        assert arc.voltages == [1.0, 1.0, 2.0]
        assert arc.main == [True, False]
        assert seconds < 1.0


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

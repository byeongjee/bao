"""Unit tests for capacitor config discovery and cap-aware analysis helpers."""

from __future__ import annotations

from pathlib import Path

from ckpt.analysis.plot import _sort_key
from ckpt.analysis.strip_mining import parse_strip_mining_log
from ckpt.bench.config import discover_capacitors
from ckpt.env import ProjectEnv


PROJECT_DIR = Path(__file__).resolve().parent.parent


def test_discover_capacitors_includes_50uf_by_default() -> None:
    env = ProjectEnv.from_environ(PROJECT_DIR)

    caps = discover_capacitors(env, "milp")

    assert [cap.label for cap in caps] == ["1uF", "5uF", "10uF", "50uF", "100uF"]


def test_discover_capacitors_resolves_50uf_explicitly() -> None:
    env = ProjectEnv.from_environ(PROJECT_DIR)

    caps = discover_capacitors(env, "milp", ["50uF"])

    assert len(caps) == 1
    assert caps[0].label == "50uF"
    assert caps[0].config_path == PROJECT_DIR / "benchmarks" / "config_50uF.json"


def test_parse_strip_mining_log_uses_50uf_capacity(tmp_path: Path) -> None:
    log_path = tmp_path / "strip_mining.log"
    log_path.write_text("[1/1] Running crc-50uF ...\n")

    runs = parse_strip_mining_log(log_path)

    assert len(runs) == 1
    assert runs[0]["capacitor"] == "50uF"
    assert runs[0]["capacity"] == 243000.0


def test_plot_sort_key_orders_capacitors_numerically() -> None:
    labels = [
        "crc\n(100uF)",
        "crc\n(50uF)",
        "crc\n(10uF)",
        "crc\n(5uF)",
        "crc\n(1uF)",
    ]

    assert sorted(labels, key=_sort_key) == [
        "crc\n(1uF)",
        "crc\n(5uF)",
        "crc\n(10uF)",
        "crc\n(50uF)",
        "crc\n(100uF)",
    ]

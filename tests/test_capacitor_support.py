"""Unit tests for capacitor config discovery and cap-aware analysis helpers."""

from __future__ import annotations

from pathlib import Path

import pytest
from ckpt.analysis.strip_mining import CAPACITY_MAP, parse_strip_mining_log
from ckpt.bench.config import _DEFAULT_CAPS, discover_capacitors
from ckpt.env import ProjectEnv
from conftest import PROJECT_DIR

pytestmark = pytest.mark.unit


def test_discover_capacitors_resolves_every_default_cap() -> None:
    """Discovery silently drops a default cap whose config file is missing."""
    env = ProjectEnv.from_environ(PROJECT_DIR)

    caps = discover_capacitors(env, "milp", None)

    assert [cap.label for cap in caps] == _DEFAULT_CAPS
    # Comparing against _DEFAULT_CAPS alone still passes if 50uF is dropped
    # from both.
    assert "50uF" in _DEFAULT_CAPS


def test_discover_capacitors_resolves_explicit_cap() -> None:
    env = ProjectEnv.from_environ(PROJECT_DIR)
    label = _DEFAULT_CAPS[-1]

    caps = discover_capacitors(env, "milp", [label])

    assert len(caps) == 1
    assert caps[0].label == label
    assert caps[0].config_path == PROJECT_DIR / "benchmarks" / f"config_{label}.json"


def test_parse_strip_mining_log_resolves_capacitor_capacity(tmp_path: Path) -> None:
    log_path = tmp_path / "strip_mining.log"
    log_path.write_text("[1/1] Running crc-50uF ...\n")

    runs = parse_strip_mining_log(log_path)

    assert len(runs) == 1
    assert runs[0]["capacitor"] == "50uF"
    assert runs[0]["capacity"] == CAPACITY_MAP["50uF"]

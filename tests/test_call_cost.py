"""SCHEMATIC charges call_cost once at every function-entry energy seed.

Faithful to the reference (schematic.py:184-186): call_cost is subtracted from
the per-function RCG energy seed (the sub-path whose front is START_Func) for
EVERY function, including the single-function case. We prove the wiring without
depending on exact energy numbers: a trivial function that fits comfortably with
call_cost=0 must become infeasible once call_cost exceeds the entry budget.
"""

from __future__ import annotations

import json

import pytest

from conftest import CONFIGS_DIR, SCENARIOS_DIR

pytestmark = pytest.mark.schematic

ENERGY_CONFIG = CONFIGS_DIR / "scenario_config.json"
BASE_SCHEMATIC_CONFIG = CONFIGS_DIR / "scenario_schematic_config.json"
SRC = SCENARIOS_DIR / "scenario_no_ckpt.c"


def _config_with_call_cost(tmp_path, call_cost: float):
    cfg = json.loads(BASE_SCHEMATIC_CONFIG.read_text())
    cfg["call_cost"] = call_cost
    path = tmp_path / f"schematic_call_cost_{call_cost}.json"
    path.write_text(json.dumps(cfg))
    return path


def test_call_cost_zero_keeps_trivial_function_feasible(run_schematic, tmp_path):
    cfg = _config_with_call_cost(tmp_path, 0.0)
    r = run_schematic(SRC, ENERGY_CONFIG, cfg, tmp_path)
    assert r.exit_code == 0
    assert "energy capacity too small" not in r.stderr


def test_call_cost_charged_at_function_entry_seed(run_schematic, tmp_path):
    # call_cost larger than the whole entry budget drives the function-level RCG
    # seed negative, so even a trivial, call-free function becomes infeasible.
    # This only happens if call_cost is actually subtracted from the START_Func
    # seed — which is the faithful per-function charge.
    cfg = _config_with_call_cost(tmp_path, 5000.0)
    r = run_schematic(SRC, ENERGY_CONFIG, cfg, tmp_path)
    assert r.exit_code == 0
    assert "energy capacity too small" in r.stderr

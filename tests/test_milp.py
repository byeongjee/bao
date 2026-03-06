"""MILP checkpoint insertion tests."""

from __future__ import annotations

import pytest

from conftest import (
    TESTS_DIR,
    check_assertions,
)

# ---------------------------------------------------------------------------
# Config paths
# ---------------------------------------------------------------------------
ESTIMATOR_UNIFORM = TESTS_DIR / "estimator_ir_uniform.json"
MILP_PARAMS = TESTS_DIR / "milp_params.json"
MILP_PARAMS_SMALL = TESTS_DIR / "milp_params_small.json"

# ---------------------------------------------------------------------------
# Module-wide mark
# ---------------------------------------------------------------------------
pytestmark = pytest.mark.milp


# ---------------------------------------------------------------------------
# Parametrized success tests
# ---------------------------------------------------------------------------
_SUCCESS_CASES = [
    ("test_linear",            "test_linear.c",            MILP_PARAMS,       {"exit": 0, "min_prologue": 1}),
    ("test_diamond",           "test_diamond.c",           MILP_PARAMS,       {"exit": 0, "min_prologue": 1}),
    ("test_simple_loop",       "test_simple_loop.c",       MILP_PARAMS,       {"exit": 0, "min_prologue": 1}),
    ("test_nested_loops",      "test_nested_loops.c",      MILP_PARAMS,       {"exit": 0, "min_prologue": 1}),
    ("test_early_return",      "test_early_return.c",      MILP_PARAMS,       {"exit": 0, "min_prologue": 1}),
    ("test_exit_constraint",   "test_exit_constraint.c",   MILP_PARAMS,       {"exit": 0, "min_prologue": 1}),
    ("test_switch",            "test_switch.c",            MILP_PARAMS,       {"exit": 0, "min_prologue": 1}),
    ("test_distributed_stores","test_distributed_stores.c",MILP_PARAMS_SMALL, {"exit": 0, "min_prologue": 1}),
    ("test_vm_nvm_placement",  "test_vm_nvm_placement.c",  MILP_PARAMS_SMALL, {"exit": 0, "min_prologue": 1}),
]


@pytest.mark.parametrize(
    "name,src_file,milp_config,expect",
    [(name, src, cfg, exp) for name, src, cfg, exp in _SUCCESS_CASES],
    ids=[name for name, *_ in _SUCCESS_CASES],
)
def test_milp_succeeds(run_milp, tmp_path_factory, name, src_file, milp_config, expect):
    tmp_path = tmp_path_factory.mktemp(name)
    result = run_milp(
        TESTS_DIR / src_file,
        ESTIMATOR_UNIFORM,
        milp_config,
        tmp_path,
    )
    check_assertions(result, expect)


# ---------------------------------------------------------------------------
# Infeasible test
# ---------------------------------------------------------------------------
def test_milp_infeasible(run_milp, tmp_path):
    result = run_milp(
        TESTS_DIR / "test_infeasible.c",
        ESTIMATOR_UNIFORM,
        MILP_PARAMS_SMALL,
        tmp_path,
    )
    check_assertions(result, {
        "exit": 0,
        "stderr_contains": "exceed energy capacity",
    })


# ---------------------------------------------------------------------------
# Missing bb-freq-file test
# ---------------------------------------------------------------------------
def test_milp_missing_bb_freq(run_milp, tmp_path):
    result = run_milp(
        TESTS_DIR / "test_linear.c",
        ESTIMATOR_UNIFORM,
        MILP_PARAMS,
        tmp_path,
        skip_bb_freq=True,
    )
    check_assertions(result, {
        "exit": "nonzero",
        "stderr_contains": "bb-freq-file",
    })

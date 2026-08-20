"""MILP checkpoint insertion tests."""

from __future__ import annotations

import pytest
from conftest import (
    CONFIGS_DIR,
    SCENARIOS_DIR,
    TESTS_DIR,
    check_assertions,
    get_metric,
    require_metric,
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
    ("test_linear", "test_linear.c", MILP_PARAMS, {"exit": 0, "min_prologue": 1}),
    ("test_diamond", "test_diamond.c", MILP_PARAMS, {"exit": 0, "min_prologue": 1}),
    (
        "test_simple_loop",
        "test_simple_loop.c",
        MILP_PARAMS,
        {"exit": 0, "min_prologue": 1},
    ),
    (
        "test_nested_loops",
        "test_nested_loops.c",
        MILP_PARAMS,
        {"exit": 0, "min_prologue": 1},
    ),
    (
        "test_early_return",
        "test_early_return.c",
        MILP_PARAMS,
        {"exit": 0, "min_prologue": 1},
    ),
    (
        "test_exit_constraint",
        "test_exit_constraint.c",
        MILP_PARAMS,
        {"exit": 0, "min_prologue": 1},
    ),
    ("test_switch", "test_switch.c", MILP_PARAMS, {"exit": 0, "min_prologue": 1}),
    (
        "test_distributed_stores",
        "test_distributed_stores.c",
        MILP_PARAMS_SMALL,
        {"exit": 0, "min_prologue": 1},
    ),
    (
        "test_vm_nvm_placement",
        "test_vm_nvm_placement.c",
        MILP_PARAMS_SMALL,
        {"exit": 0, "min_prologue": 1},
    ),
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
    check_assertions(
        result,
        {
            "exit": 0,
            "stderr_contains": "exceed energy capacity",
        },
    )


# ---------------------------------------------------------------------------
# Multi-base pointer test
# ---------------------------------------------------------------------------
def test_milp_keeps_multi_base_pointer_globals_in_nvm(run_milp, tmp_path):
    """test_mixed_base_ptr.c: an access whose pointer can root at more than
    one global cannot be redirected to a single VM shadow, so the globals it
    may alias are excluded from the candidate set and stay in NVM."""
    result = run_milp(
        TESTS_DIR / "test_mixed_base_ptr.c",
        ESTIMATOR_UNIFORM,
        MILP_PARAMS,
        tmp_path,
    )
    check_assertions(
        result,
        {
            "exit": 0,
            "stderr_contains": "keeping it in NVM",
        },
    )
    assert "__vm_shadow_a" not in result.output_ir
    assert "__vm_shadow_b" not in result.output_ir


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
    check_assertions(
        result,
        {
            "exit": "nonzero",
            "stderr_contains": "bb-freq-file",
        },
    )


def test_coarse_allocation_reduces_problem_size(run_milp, tmp_path_factory):
    fine_tmp = tmp_path_factory.mktemp("scenario_merge_divergence_fine")
    coarse_tmp = tmp_path_factory.mktemp("scenario_merge_divergence_coarse")
    src = SCENARIOS_DIR / "scenario_merge_divergence.c"
    energy_config = CONFIGS_DIR / "scenario_config.json"
    milp_config = CONFIGS_DIR / "scenario_merge_divergence_milp_config.json"

    fine = run_milp(src, energy_config, milp_config, fine_tmp)
    coarse = run_milp(
        src,
        energy_config,
        milp_config,
        coarse_tmp,
        coarse_allocation=True,
    )

    check_assertions(fine, {"exit": 0})
    check_assertions(coarse, {"exit": 0})
    assert get_metric(fine.stderr, "MILP allocation mode") == "regional"
    assert get_metric(coarse.stderr, "MILP allocation mode") == "coarse"

    fine_vars = int(require_metric(fine.stderr, "MILP variables (before presolve)"))
    fine_constraints = int(
        require_metric(fine.stderr, "MILP constraints (before presolve)")
    )
    coarse_vars = int(require_metric(coarse.stderr, "MILP variables (before presolve)"))
    coarse_constraints = int(
        require_metric(coarse.stderr, "MILP constraints (before presolve)")
    )

    assert coarse_vars < fine_vars
    assert coarse_constraints < fine_constraints

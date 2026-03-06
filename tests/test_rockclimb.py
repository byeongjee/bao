"""RockClimb checkpoint pass tests."""

from __future__ import annotations

import pytest

from conftest import (
    TESTS_DIR,
    check_assertions,
)

# ---------------------------------------------------------------------------
# Config paths
# ---------------------------------------------------------------------------
ESTIMATOR_WEIGHTED = TESTS_DIR / "estimator_ir_weighted.json"
ROCKCLIMB_PARAMS = TESTS_DIR / "rockclimb_params.json"

# ---------------------------------------------------------------------------
# Module-wide mark
# ---------------------------------------------------------------------------
pytestmark = pytest.mark.rockclimb


# ---------------------------------------------------------------------------
# Parametrized success tests
# ---------------------------------------------------------------------------
_SUCCESS_CASES = [
    (
        "test_rockclimb_linear",
        "test_rockclimb_linear.c",
        {"exit": 0, "stderr_contains": "RockClimb Metrics"},
    ),
    (
        "test_rockclimb_loop",
        "test_rockclimb_loop.c",
        {"exit": 0, "stderr_contains": "RockClimb Metrics"},
    ),
    (
        "test_rockclimb_nested",
        "test_rockclimb_nested.c",
        {"exit": 0, "stderr_contains": "RockClimb Metrics"},
    ),
    (
        "test_rockclimb_diamond",
        "test_rockclimb_diamond.c",
        {"exit": 0, "stderr_contains": "RockClimb Metrics"},
    ),
    (
        "test_rockclimb_liveout",
        "test_rockclimb_liveout.c",
        {"exit": 0, "stderr_contains": "RockClimb Metrics"},
    ),
]


@pytest.mark.parametrize(
    "name,src_file,expect",
    [(name, src, exp) for name, src, exp in _SUCCESS_CASES],
    ids=[name for name, *_ in _SUCCESS_CASES],
)
def test_rockclimb_succeeds(run_rockclimb, tmp_path_factory, name, src_file, expect):
    tmp_path = tmp_path_factory.mktemp(name)
    result = run_rockclimb(
        TESTS_DIR / src_file,
        ESTIMATOR_WEIGHTED,
        ROCKCLIMB_PARAMS,
        tmp_path,
    )
    check_assertions(result, expect)

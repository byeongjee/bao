"""Energy validation tests for checkpoint insertion.

Absorbs: test_validate.sh, test_validate_milp.sh, test_validate_energy_regression.sh
"""

from __future__ import annotations

import subprocess

import pytest

from conftest import (
    ENERGY_VALIDATE_RUNTIME,
    TESTS_DIR,
)

pytestmark = [pytest.mark.milp, pytest.mark.energy_validation]

# ---------------------------------------------------------------------------
# Config paths
# ---------------------------------------------------------------------------
ESTIMATOR_CONFIG = TESTS_DIR / "estimator_ir_uniform.json"
MILP_CONFIG = TESTS_DIR / "milp_params.json"


# ---------------------------------------------------------------------------
# Test 1: User-placed checkpoints — passing case (every 5 iters)
# ---------------------------------------------------------------------------
def test_user_checkpoint_pass(tools, tmp_path):
    """test_validate_pass.c: checkpoints every 5 iters should not violate energy."""
    src = TESTS_DIR / "test_validate_pass.c"
    input_ll = tmp_path / "test.ll"
    validated_ll = tmp_path / "validated.ll"
    test_bin = tmp_path / "test_bin"

    # Compile to IR
    r = subprocess.run(
        [
            tools["clang"],
            *tools["sysroot_flags"],
            "-S", "-emit-llvm", "-O0", "-Xclang", "-disable-O0-optnone",
            str(src), "-o", str(input_ll),
        ],
        capture_output=True, text=True,
    )
    assert r.returncode == 0, f"clang failed: {r.stderr}"

    # Run energy-validate pass
    r = subprocess.run(
        [
            tools["opt"],
            f"-load-pass-plugin={tools['pass_lib']}",
            "-passes=energy-validate",
            f"-energy-config={ESTIMATOR_CONFIG}",
            f"-milp-config={MILP_CONFIG}",
            "-validate-checkpoint-function=checkpoint",
            "-S", str(input_ll), "-o", str(validated_ll),
        ],
        capture_output=True, text=True,
    )
    assert r.returncode == 0, f"energy-validate pass failed: {r.stderr}"

    # Compile and link with validation runtime
    r = subprocess.run(
        [
            tools["clang"],
            *tools["sysroot_flags"],
            "-O0",
            str(validated_ll), str(ENERGY_VALIDATE_RUNTIME),
            "-o", str(test_bin),
        ],
        capture_output=True, text=True,
    )
    assert r.returncode == 0, f"final compile failed: {r.stderr}"

    # Run — expect clean exit
    r = subprocess.run([str(test_bin)], capture_output=True, text=True)
    assert r.returncode == 0, (
        f"Expected exit 0 (no energy violation) but got {r.returncode}.\n"
        f"stderr: {r.stderr[:500]}"
    )


# ---------------------------------------------------------------------------
# Test 2: User-placed checkpoints — failing case (every 50 iters)
# ---------------------------------------------------------------------------
def test_user_checkpoint_fail(tools, tmp_path):
    """test_validate_fail.c: checkpoints every 50 iters should trigger ENERGY VIOLATION."""
    src = TESTS_DIR / "test_validate_fail.c"
    input_ll = tmp_path / "test.ll"
    validated_ll = tmp_path / "validated.ll"
    test_bin = tmp_path / "test_bin"

    # Compile to IR
    r = subprocess.run(
        [
            tools["clang"],
            *tools["sysroot_flags"],
            "-S", "-emit-llvm", "-O0", "-Xclang", "-disable-O0-optnone",
            str(src), "-o", str(input_ll),
        ],
        capture_output=True, text=True,
    )
    assert r.returncode == 0, f"clang failed: {r.stderr}"

    # Run energy-validate pass
    r = subprocess.run(
        [
            tools["opt"],
            f"-load-pass-plugin={tools['pass_lib']}",
            "-passes=energy-validate",
            f"-energy-config={ESTIMATOR_CONFIG}",
            f"-milp-config={MILP_CONFIG}",
            "-validate-checkpoint-function=checkpoint",
            "-S", str(input_ll), "-o", str(validated_ll),
        ],
        capture_output=True, text=True,
    )
    assert r.returncode == 0, f"energy-validate pass failed: {r.stderr}"

    # Compile and link with validation runtime
    r = subprocess.run(
        [
            tools["clang"],
            *tools["sysroot_flags"],
            "-O0",
            str(validated_ll), str(ENERGY_VALIDATE_RUNTIME),
            "-o", str(test_bin),
        ],
        capture_output=True, text=True,
    )
    assert r.returncode == 0, f"final compile failed: {r.stderr}"

    # Run — expect non-zero exit with ENERGY VIOLATION message
    r = subprocess.run([str(test_bin)], capture_output=True, text=True)
    assert r.returncode != 0, (
        "Expected non-zero exit (energy violation) but got exit 0.\n"
        f"stdout: {r.stdout[:200]}"
    )
    assert "ENERGY VIOLATION" in r.stderr, (
        f"Expected 'ENERGY VIOLATION' in stderr.\nstderr: {r.stderr[:500]}"
    )


# ---------------------------------------------------------------------------
# Test 3: MILP-inserted checkpoints validated end-to-end (parametrized)
# ---------------------------------------------------------------------------
_MILP_VALIDATE_CASES = [
    "test_linear.c",
    "test_diamond.c",
    "test_simple_loop.c",
]


@pytest.mark.parametrize("src_name", _MILP_VALIDATE_CASES)
def test_milp_energy_validated(tools, run_milp, tmp_path_factory, src_name):
    """MILP-inserted checkpoints must not trigger energy violations at runtime."""
    tmp_path = tmp_path_factory.mktemp(f"milp_validate_{src_name.replace('.c', '')}")
    src = TESTS_DIR / src_name

    # Step 1: Run full MILP pipeline (compile + bb-freq + checkpoint)
    milp_result = run_milp(src, ESTIMATOR_CONFIG, MILP_CONFIG, tmp_path)
    assert milp_result.exit_code == 0, (
        f"MILP pass failed on {src_name}: {milp_result.stderr[:500]}"
    )

    # Step 2: Write MILP output to a file for energy-validate
    milp_ll = tmp_path / "milp_output.ll"
    milp_ll.write_text(milp_result.output_ir)

    validated_ll = tmp_path / "validated.ll"
    test_bin = tmp_path / "test_bin"

    # Step 3: Run energy-validate pass on MILP output
    r = subprocess.run(
        [
            tools["opt"],
            f"-load-pass-plugin={tools['pass_lib']}",
            "-passes=energy-validate",
            f"-energy-config={ESTIMATOR_CONFIG}",
            f"-milp-config={MILP_CONFIG}",
            "-S", str(milp_ll), "-o", str(validated_ll),
        ],
        capture_output=True, text=True,
    )
    assert r.returncode == 0, f"energy-validate pass failed on {src_name}: {r.stderr[:500]}"

    # Step 4: Compile and link with validation runtime
    r = subprocess.run(
        [
            tools["clang"],
            *tools["sysroot_flags"],
            "-O0",
            str(validated_ll), str(ENERGY_VALIDATE_RUNTIME),
            "-o", str(test_bin),
        ],
        capture_output=True, text=True,
    )
    assert r.returncode == 0, f"final compile failed on {src_name}: {r.stderr[:500]}"

    # Step 5: Run — expect clean exit (no energy violation)
    r = subprocess.run([str(test_bin)], capture_output=True, text=True)
    assert r.returncode == 0, (
        f"MILP solution for {src_name} has energy violation at runtime "
        f"(exit={r.returncode}).\nstderr: {r.stderr[:500]}"
    )

"""Runtime test for RockClimb PHI-defined live-out checkpointing."""

from __future__ import annotations

import subprocess

import pytest

from conftest import TESTS_DIR

# ---------------------------------------------------------------------------
# Module-wide mark
# ---------------------------------------------------------------------------
pytestmark = [pytest.mark.rockclimb]

# ---------------------------------------------------------------------------
# Config paths
# ---------------------------------------------------------------------------
ESTIMATOR_WEIGHTED = TESTS_DIR / "estimator_ir_weighted.json"
ROCKCLIMB_PARAMS = TESTS_DIR / "rockclimb_params.json"


def _run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    """Run a subprocess, capturing stdout+stderr."""
    return subprocess.run(cmd, capture_output=True, text=True, **kwargs)


# ---------------------------------------------------------------------------
# Runtime test: PHI-defined live-out checkpointing
# ---------------------------------------------------------------------------
def test_rockclimb_phi_liveout_runtime(tools, tmp_path):
    """Compile, instrument, link, and execute the PHI live-out runtime test.

    Mirrors test_rockclimb_phi_liveout.sh:
      1. Compile C to IR
      2. Run mem2reg (creates PHI nodes)
      3. Assert PHI nodes exist
      4. Run RockClimb pass
      5. Compile instrumented IR to object
      6. Compile driver to object
      7. Link
      8. Execute -- exit 0 means pass
    """
    test_c = TESTS_DIR / "test_rockclimb_phi_liveout.c"
    driver_c = TESTS_DIR / "test_rockclimb_phi_liveout_driver.c"

    raw_ll = str(tmp_path / "phi_liveout_raw.ll")
    ssa_ll = str(tmp_path / "phi_liveout_ssa.ll")
    out_ll = str(tmp_path / "phi_liveout_out.ll")
    out_obj = str(tmp_path / "phi_liveout_out.o")
    driver_obj = str(tmp_path / "phi_liveout_driver.o")
    executable = str(tmp_path / "phi_liveout_test")

    # Step 1: Compile test function to LLVM IR
    r = _run([
        tools["clang"], *tools["sysroot_flags"],
        "-S", "-emit-llvm", "-O0", "-Xclang", "-disable-O0-optnone",
        str(test_c), "-o", raw_ll,
    ])
    assert r.returncode == 0, f"clang IR compile failed:\n{r.stderr}"

    # Step 2: Promote to SSA (mem2reg) to create PHI nodes
    r = _run([tools["opt"], "-passes=mem2reg", "-S", raw_ll, "-o", ssa_ll])
    assert r.returncode == 0, f"mem2reg failed:\n{r.stderr}"

    # Step 3: Verify PHI nodes exist in the promoted IR
    ssa_text = (tmp_path / "phi_liveout_ssa.ll").read_text()
    assert "= phi " in ssa_text, (
        "No PHI nodes found after mem2reg -- test source is invalid"
    )

    # Step 4: Run RockClimb pass
    r = _run([
        tools["opt"], "-load-pass-plugin", tools["pass_lib"],
        "-passes=rockclimb",
        f"-energy-config={ESTIMATOR_WEIGHTED}",
        f"-rockclimb-config={ROCKCLIMB_PARAMS}",
        "-S", ssa_ll, "-o", out_ll,
    ])
    assert r.returncode == 0, f"RockClimb pass failed:\n{r.stderr}"

    # Step 5: Compile instrumented IR to object (use system clang for native linking)
    sys_clang = "/usr/bin/clang"
    r = _run([sys_clang, "-c", out_ll, "-o", out_obj])
    assert r.returncode == 0, f"Compile instrumented IR to object failed:\n{r.stderr}"

    # Step 6: Compile runtime driver to object
    r = _run([sys_clang, "-c", str(driver_c), "-o", driver_obj])
    assert r.returncode == 0, f"Compile driver failed:\n{r.stderr}"

    # Step 7: Link
    r = _run([sys_clang, out_obj, driver_obj, "-o", executable])
    assert r.returncode == 0, f"Link failed:\n{r.stderr}"

    # Step 8: Run -- exit 0 means the PHI-defined value was correctly checkpointed
    r = _run([executable])
    assert r.returncode == 0, (
        f"Runtime test failed (exit {r.returncode}).\n"
        f"stdout: {r.stdout}\nstderr: {r.stderr}"
    )

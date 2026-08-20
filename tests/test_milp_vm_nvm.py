"""VM/NVM placement enforcement tests for the MILP checkpoint pass.

Absorbs: test_vm_nvm_enforcement.sh
"""

from __future__ import annotations

import re

import pytest
from conftest import (
    CONFIGS_DIR,
    SCENARIOS_DIR,
    TESTS_DIR,
    check_assertions,
    has_global,
    has_section,
)

pytestmark = [pytest.mark.milp, pytest.mark.vm_nvm]


# ---------------------------------------------------------------------------
# Test 1: VM/NVM enforcement — hot + cold globals
# ---------------------------------------------------------------------------
def test_vm_nvm_enforcement(run_milp, tmp_path):
    """scenario_vm_nvm_enforcement.c: hot global gets shadow and rewritten accesses."""
    src = SCENARIOS_DIR / "scenario_vm_nvm_enforcement.c"
    energy_config = CONFIGS_DIR / "scenario_config.json"
    milp_config = CONFIGS_DIR / "scenario_milp_config.json"

    result = run_milp(src, energy_config, milp_config, tmp_path)

    # Pass must succeed
    check_assertions(result, {"exit": 0, "has_shadow": ["g_hot"]})

    ir = result.output_ir

    # Both globals get .fram section
    assert has_section(ir, "g_hot", ".fram"), '@g_hot must have section ".fram"'
    assert has_section(ir, "g_cold", ".fram"), '@g_cold must have section ".fram"'

    # Shadow global for the hot variable must exist
    assert has_global(ir, "__vm_shadow_g_hot"), (
        "Expected shadow global @__vm_shadow_g_hot in IR"
    )

    # Accesses to g_hot must be rewritten to the shadow
    assert "store i32 1, ptr @__vm_shadow_g_hot" in ir, (
        "Expected 'store i32 1, ptr @__vm_shadow_g_hot' in IR "
        "(access rewrite to shadow)"
    )

    # No direct store into @g_hot inside a function body
    # (global-level initializer lines start at column 0 with "@g_hot =",
    # whereas stores in function bodies are indented)
    direct_store = re.search(r"^\s+store\s+.*ptr\s+@g_hot\b", ir, re.MULTILINE)
    assert direct_store is None, (
        "Found unexpected direct store to @g_hot in function body — "
        "accesses should be rewritten to the shadow"
    )


# ---------------------------------------------------------------------------
# Test 2: VM overflow — capacity constraint respected
# ---------------------------------------------------------------------------
def test_vm_overflow(run_milp, tmp_path):
    """scenario_vm_overflow.c: with vm_capacity=16 and 5x4-byte globals, <=4 get shadows."""
    src = SCENARIOS_DIR / "scenario_vm_overflow.c"
    energy_config = CONFIGS_DIR / "scenario_vm_overflow_config.json"
    milp_config = CONFIGS_DIR / "scenario_milp_vm_overflow_config.json"

    result = run_milp(src, energy_config, milp_config, tmp_path)

    # Pass must succeed
    check_assertions(result, {"exit": 0})

    ir = result.output_ir

    # All 5 globals must be placed in NVM
    for g in ("g_a", "g_b", "g_c", "g_d", "g_e"):
        assert has_section(ir, g, ".fram"), f'@{g} must have section ".fram"'

    # With vm_capacity=16 bytes and 5 globals x 4 bytes, at most 4 fit in VM
    shadow_count = len(re.findall(r"@__vm_shadow_g_\w+\s*=\s*internal\s+global", ir))
    assert shadow_count <= 4, (
        f"Shadow count {shadow_count} exceeds VM capacity (max 4 of 5 globals)"
    )

    # At least one shadow must exist (some globals are still hot enough for VM)
    assert shadow_count >= 1, (
        "Expected at least one shadow global — VM should be used for hot globals"
    )

    # At least one global must remain NVM-only (no shadow allocated)
    shadowed = set(re.findall(r"@__vm_shadow_(g_[a-e])\b", ir))
    nvm_only = {g for g in ("g_a", "g_b", "g_c", "g_d", "g_e") if g not in shadowed}
    assert len(nvm_only) >= 1, (
        "All 5 globals have shadows — VM capacity constraint not enforced"
    )


# ---------------------------------------------------------------------------
# Test 3: Basic VM/NVM placement on existing test file
# ---------------------------------------------------------------------------
def test_vm_nvm_basic(run_milp, tmp_path):
    """test_vm_nvm_placement.c: frequently_accessed gets .fram section and a shadow."""
    src = TESTS_DIR / "test_vm_nvm_placement.c"
    energy_config = TESTS_DIR / "estimator_ir_uniform.json"
    milp_config = TESTS_DIR / "milp_params_small.json"

    result = run_milp(src, energy_config, milp_config, tmp_path)

    check_assertions(result, {"exit": 0})

    ir = result.output_ir

    # At least one global must carry a .fram section annotation
    assert 'section ".fram"' in ir, (
        'Expected at least one global with section ".fram" in IR'
    )

    # At least one shadow global must be present
    assert "__vm_shadow_" in ir, "Expected at least one @__vm_shadow_* global in IR"


def test_vm_nvm_basic_with_coarse_allocation(run_milp, tmp_path):
    """scenario_vm_hot.c: coarse allocation still places a hot global in VM."""
    src = SCENARIOS_DIR / "scenario_vm_hot.c"
    energy_config = CONFIGS_DIR / "scenario_config.json"
    milp_config = CONFIGS_DIR / "scenario_milp_config.json"

    result = run_milp(
        src,
        energy_config,
        milp_config,
        tmp_path,
        coarse_allocation=True,
    )

    check_assertions(result, {"exit": 0, "has_shadow": ["g_hot"]})


# ---------------------------------------------------------------------------
# Test 4: Pointer-IV access to a VM-placed global (phi-based pointer)
# ---------------------------------------------------------------------------
def test_vm_rewrite_through_pointer_phi(run_milp, tmp_path):
    """scenario_ptr_iv_vm.c: phi-derived accesses to a VM-placed array are rewritten.

    The loop walks g_hot via a pointer induction variable, so its accesses
    reach the global only through a phi. Candidate identification must
    resolve the single-base phi (not exclude the global), and the instrumenter must
    redirect the accesses to the shadow via the runtime-offset form
    shadow + (p - g_hot).
    """
    src = SCENARIOS_DIR / "scenario_ptr_iv_vm.c"
    energy_config = CONFIGS_DIR / "scenario_config.json"
    milp_config = CONFIGS_DIR / "scenario_ptr_iv_vm_milp_config.json"

    result = run_milp(src, energy_config, milp_config, tmp_path)

    check_assertions(result, {"exit": 0, "has_shadow": ["g_hot"]})

    ir = result.output_ir

    # The phi-based accesses must be redirected via the runtime-offset form
    # (vm.off/vm.addr are only emitted for derived pointers).
    assert "vm.off" in ir and "vm.addr" in ir, (
        "Expected phi-derived accesses to be rewritten as "
        "shadow + (p - g_hot) (vm.off/vm.addr in IR)"
    )
    assert "getelementptr i8, ptr @__vm_shadow_g_hot" in ir, (
        "Expected the rewritten address to be based on @__vm_shadow_g_hot"
    )

"""End-to-end inter-procedural SCHEMATIC tests (P3: DISABLED fold).

Runs the full inter-procedural pipeline on a multi-function module:
    clang -O0 -> tripcount-annotation -> schematic-isolate
              -> trace-collect (on isolated IR) -> schematic (module pass)

Before inter-procedural support, the SCHEMATIC pass rejected any caller with a
call to a defined function ("unresolved memory/call effects") and skipped it.
With call isolation + the bottom-up fold, callers are analyzed with each callee's
summary folded onto its call site. A checkpoint-free callee folds in the DISABLED
regime (transparent): the callee energy rides on call_entry; the call edge is a
DISABLED checkpoint, never an active boundary.
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

import pytest
from conftest import (
    CONFIGS_DIR,
    SCENARIOS_DIR,
    PassResult,
    _collect_schematic_trace,
    _run,
)

pytestmark = pytest.mark.schematic

ENERGY_CONFIG = CONFIGS_DIR / "scenario_config.json"
SCHEMATIC_CONFIG = CONFIGS_DIR / "scenario_schematic_config.json"
FUNC_ALL_NVM = SCENARIOS_DIR / "func_all_nvm.c"
FUNC_VIRTUAL = SCENARIOS_DIR / "func_virtual.c"
FUNC_SHARED_VM = SCENARIOS_DIR / "func_shared_vm.c"
FUNC_INFRA_CALL = SCENARIOS_DIR / "func_infra_call.c"
FUNC_CALL_IN_LOOP = SCENARIOS_DIR / "func_call_in_loop.c"
FUNC_CALL_CHAIN = SCENARIOS_DIR / "func_call_chain.c"


def _func_stats(stderr: str, fn: str) -> dict[str, str]:
    """Parse the per-function SCHEMATIC stats block out of stderr."""
    # Each function prints a block starting at "Function:   <name>".
    blocks = re.split(r"=== Checkpoint Insertion Statistics ===", stderr)
    for b in blocks:
        m = re.search(r"^\s*Function:\s+(\S+)\s*$", b, re.MULTILINE)
        if m and m.group(1) == fn:
            out = {}
            for line in b.splitlines():
                mm = re.match(r"^\s{2}([A-Za-z][^:]*?):\s+(.+?)\s*$", line)
                if mm:
                    out[mm.group(1).strip()] = mm.group(2).strip()
            return out
    return {}


def _run_schematic_interproc(
    tools, compile_to_ir, src, energy_config, schematic_config, tmp_path
):
    """Full inter-procedural pipeline: isolate, trace on isolated IR, then solve."""
    energy_config = str(energy_config)
    schematic_config = str(schematic_config)

    input_ll = str(tmp_path / "input.ll")
    compile_to_ir(src, input_ll, mem2reg=False)

    ann_ll = str(tmp_path / "annotated.ll")
    r = _run(
        [
            tools["opt"],
            "-load-pass-plugin",
            tools["pass_lib"],
            "-passes=tripcount-annotation",
            "-S",
            input_ll,
            "-o",
            ann_ll,
        ]
    )
    assert r.returncode == 0, f"tripcount-annotation failed: {r.stderr}"

    iso_ll = str(tmp_path / "isolated.ll")
    r = _run(
        [
            tools["opt"],
            "-load-pass-plugin",
            tools["pass_lib"],
            "-passes=schematic-isolate",
            "-S",
            ann_ll,
            "-o",
            iso_ll,
        ]
    )
    assert r.returncode == 0, f"schematic-isolate failed: {r.stderr}"

    trace_json = _collect_schematic_trace(tools, iso_ll, energy_config, tmp_path)

    output_ll = str(tmp_path / "output.ll")
    r = subprocess.run(
        [
            tools["opt"],
            "-load-pass-plugin",
            tools["pass_lib"],
            "-passes=schematic",
            f"-energy-config={energy_config}",
            f"-schematic-config={schematic_config}",
            f"-schematic-trace={trace_json}",
            "-S",
            iso_ll,
            "-o",
            output_ll,
        ],
        capture_output=True,
        text=True,
        timeout=120,
        check=False,
    )
    output_ir = Path(output_ll).read_text() if Path(output_ll).exists() else ""
    return PassResult(r.returncode, r.stdout, r.stderr, output_ir)


def test_disabled_fold_caller_is_analyzed(tools, compile_to_ir, tmp_path):
    """main and g (each contain a call) are analyzed, not skipped."""
    r = _run_schematic_interproc(
        tools, compile_to_ir, FUNC_ALL_NVM, ENERGY_CONFIG, SCHEMATIC_CONFIG, tmp_path
    )
    assert r.exit_code == 0, r.stderr

    # The pre-inter-procedural behavior: a caller with a call to a defined
    # function is rejected. That must no longer happen.
    assert "unresolved memory/call effects" not in r.stderr, r.stderr

    # All three functions are solved bottom-up (f, then g, then main).
    for fn in ("f", "g", "main"):
        assert f"Function:                        {fn}\n" in r.stderr, (
            f"function '{fn}' was not analyzed\n{r.stderr}"
        )


def test_disabled_fold_no_active_checkpoint_at_call(tools, compile_to_ir, tmp_path):
    """Checkpoint-free callees fold DISABLED: no region boundary is emitted."""
    r = _run_schematic_interproc(
        tools, compile_to_ir, FUNC_ALL_NVM, ENERGY_CONFIG, SCHEMATIC_CONFIG, tmp_path
    )
    assert r.exit_code == 0, r.stderr
    # Large budget -> everything fits in one region -> no checkpoints anywhere,
    # and in particular none on the call edges (DISABLED, not Active).
    assert "energy capacity too small" not in r.stderr, r.stderr
    assert r.output_ir.count("call void @__region_boundary()") == 0, r.output_ir


def test_virtual_callee_has_checkpoint_caller_stays_feasible(
    tools, compile_to_ir, tmp_path
):
    """A callee that needs an internal checkpoint folds VIRTUAL.

    heavy() far exceeds the budget and is split internally (its own boundary).
    main calls heavy: the call becomes a wall, so main stays feasible. If the
    callee energy were instead baked transparently onto one interior block in
    main, main could not fit it and would be infeasible.
    """
    r = _run_schematic_interproc(
        tools, compile_to_ir, FUNC_VIRTUAL, ENERGY_CONFIG, SCHEMATIC_CONFIG, tmp_path
    )
    assert r.exit_code == 0, r.stderr
    assert "energy capacity too small" not in r.stderr, r.stderr

    # The callee carries the real boundary: it is split internally.
    heavy = _func_stats(r.stderr, "heavy")
    assert heavy, f"heavy was not analyzed\n{r.stderr}"
    assert int(heavy["Region boundaries"]) >= 1, f"heavy should be split: {heavy}"

    # main is analyzed and feasible despite heavy >> capacity (the call is a wall).
    main = _func_stats(r.stderr, "main")
    assert main, f"main was not analyzed\n{r.stderr}"


def test_virtual_no_boundary_emitted_at_call_site(tools, compile_to_ir, tmp_path):
    """VIRTUAL is faithful: no caller-side boundary at the call (D7 no-emit).

    The real boundary lives inside heavy. main itself fits in one region around
    the wall, so main emits no __region_boundary of its own.
    """
    r = _run_schematic_interproc(
        tools, compile_to_ir, FUNC_VIRTUAL, ENERGY_CONFIG, SCHEMATIC_CONFIG, tmp_path
    )
    assert r.exit_code == 0, r.stderr
    main = _func_stats(r.stderr, "main")
    assert main.get("Region boundaries") == "0", (
        f"VIRTUAL must not emit a caller-side boundary: {main}\n{r.stderr}"
    )


def test_shared_global_vm_uses_one_module_scoped_shadow(tools, compile_to_ir, tmp_path):
    """A global VM-placed in two functions must share ONE module-scoped shadow.

    Without dedup, f and main each create their own __vm_shadow_a (the second
    auto-renamed __vm_shadow_a.1), so they cache `a` in different SRAM slots and
    a transparent call reads a stale value.
    """
    r = _run_schematic_interproc(
        tools, compile_to_ir, FUNC_SHARED_VM, ENERGY_CONFIG, SCHEMATIC_CONFIG, tmp_path
    )
    assert r.exit_code == 0, r.stderr
    shadows = re.findall(r"^@(__vm_shadow_a(?:\.\d+)?)\s*=", r.output_ir, re.MULTILINE)
    assert shadows == ["__vm_shadow_a"], (
        f"global 'a' is VM-placed in f and main; expected exactly one shared shadow "
        f"@__vm_shadow_a, got {shadows}"
    )


def test_defined_infra_callee_does_not_strand_caller(tools, compile_to_ir, tmp_path):
    """A call to a DEFINED benchmark-infra function must not strand its caller.

    The driver skips solving infra functions (debug_*/uart_*/_timing_delay*/
    timing_gpio*). Isolation must treat them as helpers (not isolate) and
    StateAnalysis must allow the call, so the caller is still analyzed instead of
    being skipped for a missing callee summary.
    """
    r = _run_schematic_interproc(
        tools, compile_to_ir, FUNC_INFRA_CALL, ENERGY_CONFIG, SCHEMATIC_CONFIG, tmp_path
    )
    assert r.exit_code == 0, r.stderr
    assert "no usable summary" not in r.stderr, r.stderr
    assert "unresolved memory/call effects" not in r.stderr, r.stderr
    assert _func_stats(r.stderr, "main"), f"main was not analyzed\n{r.stderr}"


def test_call_in_loop_folds_before_loop_analysis(tools, compile_to_ir, tmp_path):
    """A call inside the caller's loop is folded before loop analysis (D5).

    main calls step() once per loop iteration; the loop must be costed with the
    callee energy baked into the call site. main must be analyzed and feasible.
    """
    r = _run_schematic_interproc(
        tools,
        compile_to_ir,
        FUNC_CALL_IN_LOOP,
        ENERGY_CONFIG,
        SCHEMATIC_CONFIG,
        tmp_path,
    )
    assert r.exit_code == 0, r.stderr
    assert "energy capacity too small" not in r.stderr, r.stderr
    main = _func_stats(r.stderr, "main")
    assert main, f"main was not analyzed\n{r.stderr}"
    assert int(main["Loop decisions"]) >= 1, f"loop not analyzed: {main}"
    assert _func_stats(r.stderr, "step"), f"callee step not analyzed\n{r.stderr}"


def test_call_chain_propagates_virtual(tools, compile_to_ir, tmp_path):
    """Transitive VIRTUAL: b_fn (heavy) -> a_fn -> main all stay feasible.

    b_fn needs an internal checkpoint, so a_fn (which calls it) is transitively
    VIRTUAL, and main folds a_fn as a wall. All three are solved bottom-up and
    main stays feasible.
    """
    r = _run_schematic_interproc(
        tools, compile_to_ir, FUNC_CALL_CHAIN, ENERGY_CONFIG, SCHEMATIC_CONFIG, tmp_path
    )
    assert r.exit_code == 0, r.stderr
    assert "energy capacity too small" not in r.stderr, r.stderr
    b = _func_stats(r.stderr, "b_fn")
    assert b and int(b["Region boundaries"]) >= 1, (
        f"b_fn should be split (VIRTUAL): {b}"
    )
    for fn in ("a_fn", "main"):
        assert _func_stats(r.stderr, fn), f"{fn} was not analyzed\n{r.stderr}"

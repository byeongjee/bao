"""Targeted tests for the LoopStripMiningPass preprocessing rewrite."""

from __future__ import annotations

import json
import re
import subprocess

import pytest

from conftest import TESTS_DIR

pytestmark = pytest.mark.milp


def _write_milp_config(milp_config, capacity):
    milp_config.write_text(json.dumps({
        "capacity": capacity,
        "E_pro": 5.0,
        "E_epi": 3.0,
        "N_reg": 16,
        "reg_store_energy": 2.0,
        "reg_restore_energy": 2.0,
        "nvm_access_penalty": 1.5,
        "mem_store_energy_per_byte": 0.5,
        "mem_restore_energy_per_byte": 0.5,
        "vm_capacity_bytes": 2048,
        "loop_strip_mining_enabled": True,
        "loop_strip_mining_cost": 1.0,
    }))


def _write_uniform_energy_config(path, cost):
    path.write_text(json.dumps({
        "estimator_type": "ir",
        "energy_parameters": {
            "instruction_costs": {
                "simple_arithmetic": cost,
                "complex_arithmetic": cost,
                "floating_point": cost,
                "load": cost,
                "store": cost,
                "control_flow": cost,
                "comparison": cost,
                "conversion": cost,
                "call": cost,
                "phi_select": cost,
                "gep": cost,
                "alloca": cost,
                "atomic": cost,
                "default": cost,
            },
        },
    }))


def _run_preprocess(tools, compile_to_ir, src, capacity, tmp_path):
    energy_config = TESTS_DIR / "estimator_ir_uniform.json"
    milp_config = tmp_path / "milp_stripmine_divisible.json"
    _write_milp_config(milp_config, capacity)

    input_ll = tmp_path / "input.ll"
    compile_to_ir(src, input_ll, mem2reg=True)

    ann_ll = tmp_path / "annotated.ll"
    annotate = subprocess.run(
        [
            tools["opt"],
            "-load-pass-plugin",
            tools["pass_lib"],
            "-passes=tripcount-annotation",
            "-S",
            str(input_ll),
            "-o",
            str(ann_ll),
        ],
        capture_output=True,
        text=True,
    )
    assert annotate.returncode == 0, annotate.stderr

    output_ll = tmp_path / "output.ll"
    stats_json = tmp_path / "strip_stats.json"
    preprocess = subprocess.run(
        [
            tools["opt"],
            "-load-pass-plugin",
            tools["pass_lib"],
            "-passes=milp-preprocess",
            f"-energy-config={energy_config}",
            f"-milp-config={milp_config}",
            f"-loop-strip-mining-stats-json={stats_json}",
            "-S",
            str(ann_ll),
            "-o",
            str(output_ll),
        ],
        capture_output=True,
        text=True,
    )
    assert preprocess.returncode == 0, preprocess.stderr

    stats = json.loads(stats_json.read_text())
    output_ir = output_ll.read_text()
    return stats, output_ir


def _build_preprocessed_ir(tools, compile_to_ir, src, capacity, energy_config, tmp_path):
    milp_config = tmp_path / "milp_stripmine_choose.json"
    _write_milp_config(milp_config, capacity)

    input_ll = tmp_path / "input.ll"
    compile_to_ir(src, input_ll, mem2reg=True)

    ann_ll = tmp_path / "annotated.ll"
    annotate = subprocess.run(
        [
            tools["opt"],
            "-load-pass-plugin",
            tools["pass_lib"],
            "-passes=tripcount-annotation",
            "-S",
            str(input_ll),
            "-o",
            str(ann_ll),
        ],
        capture_output=True,
        text=True,
    )
    assert annotate.returncode == 0, annotate.stderr

    pre_ll = tmp_path / "pre.ll"
    preprocess = subprocess.run(
        [
            tools["opt"],
            "-load-pass-plugin",
            tools["pass_lib"],
            "-passes=milp-preprocess",
            f"-energy-config={energy_config}",
            f"-milp-config={milp_config}",
            "-S",
            str(ann_ll),
            "-o",
            str(pre_ll),
        ],
        capture_output=True,
        text=True,
    )
    assert preprocess.returncode == 0, preprocess.stderr
    return pre_ll, milp_config


def _run_choose(tools, input_ir, energy_config, milp_config, tmp_path):
    tmp_path.mkdir(parents=True, exist_ok=True)
    output_ll = tmp_path / "chosen.ll"
    choose = subprocess.run(
        [
            tools["opt"],
            "-load-pass-plugin",
            tools["pass_lib"],
            "-passes=choose-strip-mining-k",
            f"-energy-config={energy_config}",
            f"-milp-config={milp_config}",
            "-S",
            str(input_ir),
            "-o",
            str(output_ll),
        ],
        capture_output=True,
        text=True,
    )
    assert choose.returncode == 0, choose.stderr
    return choose.stderr, output_ll.read_text()


def _normalize_ir(ir):
    lines = ir.splitlines()
    if lines and lines[0].startswith("; ModuleID = "):
        lines[0] = "; ModuleID = '<normalized>'"
    return "\n".join(lines)


def _tripcount_markers(ir):
    return [int(m.group(1)) for m in re.finditer(r'llvm\.loop\.tripcount\.upper", i64 (\d+)', ir)]


def test_exact_division_strip_mining_avoids_min_select(tools, compile_to_ir, tmp_path):
    stats, output_ir = _run_preprocess(
        tools,
        compile_to_ir,
        TESTS_DIR / "test_stripmine_divisible.c",
        54.0,
        tmp_path,
    )
    chosen = stats["functions"][0]["chosen_k_values"]
    assert chosen == [{"loop_header": "for.body", "chosen_k": 2}]

    assert "outer.header" in output_ir
    assert "outer.latch" in output_ir
    assert "min.cmp" not in output_ir
    assert "inner.limit = select" not in output_ir
    assert "%exitcond = icmp ne i32 %inc, %outer.iv.plus.k" in output_ir


def test_exact_division_liveout_preserves_outer_latch_terminator(tools, compile_to_ir, tmp_path):
    _, output_ir = _run_preprocess(
        tools,
        compile_to_ir,
        TESTS_DIR / "test_stripmine_divisible_liveout.c",
        54.0,
        tmp_path,
    )

    assert "outer.header" in output_ir
    assert "outer.latch" in output_ir
    assert ".ol = phi i32" in output_ir
    assert "min.cmp" not in output_ir
    assert "inner.limit = select" not in output_ir


def test_remainder_strip_mining_uses_cleanup_loop_without_min_select(tools, compile_to_ir, tmp_path):
    stats, output_ir = _run_preprocess(
        tools,
        compile_to_ir,
        TESTS_DIR / "test_stripmine_remainder.c",
        54.0,
        tmp_path,
    )
    chosen = stats["functions"][0]["chosen_k_values"]
    assert chosen == [{"loop_header": "for.body", "chosen_k": 2}]

    assert "outer.header" in output_ir
    assert "outer.latch" in output_ir
    assert "min.cmp" not in output_ir
    assert "inner.limit = select" not in output_ir
    assert "entry.split.remainder" in output_ir
    assert "%exitcond.remainder = icmp ne i32 %inc.remainder, 13" in output_ir
    assert '!{!"llvm.loop.tripcount.upper", i64 1}' in output_ir


def test_nested_remainder_strip_mining_clones_cleanup_subloop(tools, compile_to_ir, tmp_path):
    stats, output_ir = _run_preprocess(
        tools,
        compile_to_ir,
        TESTS_DIR / "test_stripmine_nested.c",
        100.0,
        tmp_path,
    )
    chosen = stats["functions"][0]["chosen_k_values"]
    assert chosen == [{"loop_header": "for.body", "chosen_k": 2}]

    assert "outer.header" in output_ir
    assert "min.cmp" not in output_ir
    assert "inner.limit = select" not in output_ir
    assert "for.body3.remainder" in output_ir
    assert "%exitcond3.remainder = icmp ne i32 %inc6.remainder, 5" in output_ir


def test_choose_strip_mining_k_leaves_canonical_loop_unchanged(tools, compile_to_ir, tmp_path):
    energy_config = TESTS_DIR / "estimator_ir_uniform.json"
    pre_ll, milp_config = _build_preprocessed_ir(
        tools,
        compile_to_ir,
        TESTS_DIR / "test_stripmine_divisible.c",
        54.0,
        energy_config,
        tmp_path,
    )

    grow_energy = tmp_path / "energy_half.json"
    _write_uniform_energy_config(grow_energy, 0.5)
    choose_stderr, chosen_ir = _run_choose(tools, pre_ll, grow_energy, milp_config, tmp_path)

    assert "warning" not in choose_stderr.lower()
    assert _tripcount_markers(chosen_ir) == _tripcount_markers(pre_ll.read_text())
    assert "checkpoint.loop.stripmine.kind" not in chosen_ir
    assert "checkpoint.loop.no_summary" not in chosen_ir


def test_choose_strip_mining_k_can_retarget_chunked_loop(tools, compile_to_ir, tmp_path):
    energy_config = TESTS_DIR / "estimator_ir_uniform.json"
    pre_ll, milp_config = _build_preprocessed_ir(
        tools,
        compile_to_ir,
        TESTS_DIR / "test_choose_chunked_loop.c",
        54.0,
        energy_config,
        tmp_path,
    )

    pre_ir = pre_ll.read_text()
    assert "counter.check" in pre_ir
    assert "checkpoint.loop.stripmine.kind" in pre_ir
    pre_tripcounts = _tripcount_markers(pre_ir)
    assert pre_tripcounts

    grow_energy = tmp_path / "energy_half.json"
    _write_uniform_energy_config(grow_energy, 0.5)
    _, grown_ir = _run_choose(tools, pre_ll, grow_energy, milp_config, tmp_path)
    grown_tripcounts = _tripcount_markers(grown_ir)
    assert grown_tripcounts
    assert max(grown_tripcounts) > max(pre_tripcounts)

    expensive_energy = tmp_path / "energy_huge.json"
    _write_uniform_energy_config(expensive_energy, 100.0)
    choose_stderr, disabled_ir = _run_choose(
        tools,
        pre_ll,
        expensive_energy,
        milp_config,
        tmp_path / "disable_case",
    )

    assert "disabling strip-mined summary" in choose_stderr
    assert "checkpoint.loop.stripmined" not in disabled_ir
    assert "checkpoint.loop.no_summary" in disabled_ir

"""Targeted tests for the LoopStripMiningPass preprocessing rewrite."""

from __future__ import annotations

import json
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

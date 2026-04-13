"""Tests for RockClimb's IR preprocess loop-unrolling pass."""

from __future__ import annotations

import json
import subprocess
from pathlib import Path

import pytest

from ckpt.compile.rockclimb import RockClimbCompileOptions, compile_rockclimb
from ckpt.env import ProjectEnv
from ckpt.toolchain import Toolchain


PROJECT_DIR = Path(__file__).resolve().parent.parent
IR_ENERGY_CONFIG = PROJECT_DIR / "benchmarks" / "sample_energy_config_ir.json"
ASSEMBLY_ENERGY_CONFIG = PROJECT_DIR / "benchmarks" / "assembly_params.json"

pytestmark = pytest.mark.rockclimb


CONSTANT_LOOP = """\
void __loop_tripcount(int);

int sum8(int *a) {
    int s = 0;
    for (int i = 0; i < 8; i++) {
        __loop_tripcount(8);
        s += a[i];
    }
    return s;
}
"""


UNKNOWN_LOOP = """\
void __loop_tripcount(int);

int sumN(int *a, int n) {
    int s = 0;
    for (int i = 0; i < n; i++) {
        __loop_tripcount(16);
        s += a[i];
    }
    return s;
}
"""


def _run(cmd: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, capture_output=True, text=True)


def _write_src(tmp_path: Path, code: str) -> Path:
    src = tmp_path / "test.c"
    src.write_text(code)
    return src


def _write_rockclimb_config(tmp_path: Path, *, capacity: float) -> Path:
    config_path = tmp_path / "rockclimb.json"
    config_path.write_text(json.dumps({
        "capacity": capacity,
        "E_pro": 0.0,
        "E_epi": 0.0,
        "N_reg": 2,
        "reg_store_energy": 0.0,
        "reg_restore_energy": 0.0,
        "rockclimb": {"distributed_checkpointing": True},
    }))
    return config_path


def _prepare_ir_for_preprocess(tools, compile_to_ir, src: Path, tmp_path: Path) -> Path:
    input_ll = tmp_path / "input.ll"
    compile_to_ir(src, input_ll)

    annotated_ll = tmp_path / "annotated.ll"
    result = _run([
        tools["opt"], "-load-pass-plugin", tools["pass_lib"],
        "-passes=tripcount-annotation",
        "-S", str(input_ll), "-o", str(annotated_ll),
    ])
    assert result.returncode == 0, result.stderr

    optimized_ll = tmp_path / "optimized.ll"
    result = _run([
        tools["opt"],
        "-passes=default<O3>",
        "-vectorize-loops=false",
        "-vectorize-slp=false",
        "-disable-loop-unrolling",
        "-S", str(annotated_ll), "-o", str(optimized_ll),
    ])
    assert result.returncode == 0, result.stderr
    return optimized_ll


def _run_rockclimb_preprocess(tools, input_ll: Path, energy_config: Path,
                              rockclimb_config: Path, output_ll: Path) -> subprocess.CompletedProcess[str]:
    return _run([
        tools["opt"], "-load-pass-plugin", tools["pass_lib"],
        "-passes=rockclimb-preprocess",
        f"-energy-config={energy_config}",
        f"-rockclimb-config={rockclimb_config}",
        "-ckpt-log-level=info",
        "-S", str(input_ll), "-o", str(output_ll),
    ])


def _count_ir_loads(ir_text: str) -> int:
    return ir_text.count("load i32")


def test_preprocess_partially_unrolls_constant_trip_loop(tools, compile_to_ir, tmp_path):
    src = _write_src(tmp_path, CONSTANT_LOOP)
    optimized_ll = _prepare_ir_for_preprocess(tools, compile_to_ir, src, tmp_path)
    output_ll = tmp_path / "preprocessed.ll"
    config_path = _write_rockclimb_config(tmp_path, capacity=20.0)

    before_ir = optimized_ll.read_text()
    result = _run_rockclimb_preprocess(
        tools, optimized_ll, IR_ENERGY_CONFIG, config_path, output_ll,
    )

    assert result.returncode == 0, result.stderr
    after_ir = output_ll.read_text()

    assert "RockClimbLoopUnrollPass: unrolled sum8::" in result.stdout + result.stderr
    assert _count_ir_loads(after_ir) > _count_ir_loads(before_ir)


def test_preprocess_skips_loop_that_fits_budget(tools, compile_to_ir, tmp_path):
    src = _write_src(tmp_path, CONSTANT_LOOP)
    optimized_ll = _prepare_ir_for_preprocess(tools, compile_to_ir, src, tmp_path)
    output_ll = tmp_path / "preprocessed.ll"
    config_path = _write_rockclimb_config(tmp_path, capacity=200.0)

    before_ir = optimized_ll.read_text()
    result = _run_rockclimb_preprocess(
        tools, optimized_ll, IR_ENERGY_CONFIG, config_path, output_ll,
    )

    assert result.returncode == 0, result.stderr
    assert "RockClimbLoopUnrollPass: unrolled" not in result.stdout + result.stderr
    assert _count_ir_loads(output_ll.read_text()) == _count_ir_loads(before_ir)


def test_preprocess_skips_unknown_trip_count_loop(tools, compile_to_ir, tmp_path):
    src = _write_src(tmp_path, UNKNOWN_LOOP)
    optimized_ll = _prepare_ir_for_preprocess(tools, compile_to_ir, src, tmp_path)
    output_ll = tmp_path / "preprocessed.ll"
    config_path = _write_rockclimb_config(tmp_path, capacity=15.0)

    before_ir = optimized_ll.read_text()
    result = _run_rockclimb_preprocess(
        tools, optimized_ll, IR_ENERGY_CONFIG, config_path, output_ll,
    )

    assert result.returncode == 0, result.stderr
    assert "RockClimbLoopUnrollPass: unrolled" not in result.stdout + result.stderr
    assert _count_ir_loads(output_ll.read_text()) == _count_ir_loads(before_ir)


def test_compile_rockclimb_runs_preprocess(tmp_path):
    env = ProjectEnv.from_environ(PROJECT_DIR)
    tc = Toolchain.resolve(env)

    src = _write_src(tmp_path, CONSTANT_LOOP)
    config_path = _write_rockclimb_config(tmp_path, capacity=30.0)

    result = compile_rockclimb(
        tc, env,
        RockClimbCompileOptions(
            input_c=src,
            energy_config=ASSEMBLY_ENERGY_CONFIG,
            rockclimb_config=config_path,
            output=tmp_path / "rockclimb",
            pass_log_level="info",
            precomputed_energy=False,
            link=False,
            device_debug=False,
            halt_mode="nop",
            cpu_freq=1_000_000,
            clang_opt_level=3,
            opt_level=3,
            save_temps=True,
        ),
    )

    assert "RockClimbLoopUnrollPass: unrolled sum8::" in result.pass_output
    assert (tmp_path / "preprocessed.ll").exists()

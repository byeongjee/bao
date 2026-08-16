"""Tests for RockClimb's IR preprocess loop-unrolling pass."""

from __future__ import annotations

import json
import re
import subprocess
from pathlib import Path

import ckpt.compile.rockclimb as rockclimb_module
import pytest
from ckpt.compile.rockclimb import RockClimbCompileOptions, compile_rockclimb
from ckpt.env import ProjectEnv
from ckpt.runner import StepResult
from ckpt.toolchain import Toolchain
from conftest import PROJECT_DIR, TESTS_DIR, _run, write_src

# Both energy configs are owned by the test suite. Their counterparts under
# benchmarks/ are regenerated from board measurements, which would silently move
# every capacity threshold below.
IR_ENERGY_CONFIG = TESTS_DIR / "estimator_ir_weighted.json"
ASSEMBLY_ENERGY_CONFIG = TESTS_DIR / "assembly_params.json"
# One iteration of CONSTANT_LOOP under IR_ENERGY_CONFIG; its 8 iterations
# therefore cost 8x this.
CONSTANT_LOOP_ITER_ENERGY = 12.0
CRC_BENCHMARK = PROJECT_DIR / "benchmarks" / "intermittent" / "crc.c"
AES_BENCHMARK = PROJECT_DIR / "benchmarks" / "intermittent" / "aes.c"
# Capacitor energies matching the 1uF and 5uF board configs, rounded and owned
# by the test suite: benchmarks/config_*.json carries measured E_pro/E_epi and
# register costs that are re-derived whenever the board is re-characterized.
SMALL_CAP_CAPACITY = 3640.0
LARGE_CAP_CAPACITY = 18200.0

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


HELPER_LOOP = """\
void __loop_tripcount(int);

static int add1(int x) {
    return x + 1;
}

int sum8_helper(int *a) {
    int s = 0;
    for (int i = 0; i < 8; i++) {
        __loop_tripcount(8);
        s += add1(a[i]);
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


def _write_rockclimb_config(tmp_path: Path, *, capacity: float) -> Path:
    config_path = tmp_path / "rockclimb.json"
    config_path.write_text(
        json.dumps(
            {
                "capacity": capacity,
                "E_pro": 0.0,
                "E_epi": 0.0,
                "N_reg": 2,
                "reg_store_energy": 0.0,
                "reg_restore_energy": 0.0,
                "rockclimb": {"distributed_checkpointing": True},
            }
        )
    )
    return config_path


def _write_board_like_config(tmp_path: Path, *, capacity: float) -> Path:
    """Config in the regime a real capacitor puts the pass in: 16 registers to
    checkpoint and non-trivial prologue/epilogue costs, so E_safe sits just
    below capacity rather than at it."""
    config_path = tmp_path / "rockclimb_board.json"
    config_path.write_text(
        json.dumps(
            {
                "capacity": capacity,
                "E_pro": 45.0,
                "E_epi": 35.0,
                "N_reg": 16,
                "reg_store_energy": 0.0,
                "reg_restore_energy": 2.0,
                "rockclimb": {"distributed_checkpointing": True},
            }
        )
    )
    return config_path


def _prepare_ir_for_preprocess(tools, compile_to_ir, src: Path, tmp_path: Path) -> Path:
    input_ll = tmp_path / "input.ll"
    compile_to_ir(src, input_ll)

    annotated_ll = tmp_path / "annotated.ll"
    result = _run(
        [
            tools["opt"],
            "-load-pass-plugin",
            tools["pass_lib"],
            "-passes=tripcount-annotation",
            "-S",
            str(input_ll),
            "-o",
            str(annotated_ll),
        ]
    )
    assert result.returncode == 0, result.stderr

    optimized_ll = tmp_path / "optimized.ll"
    result = _run(
        [
            tools["opt"],
            "-passes=default<O3>",
            "-vectorize-loops=false",
            "-vectorize-slp=false",
            "-disable-loop-unrolling",
            "-S",
            str(annotated_ll),
            "-o",
            str(optimized_ll),
        ]
    )
    assert result.returncode == 0, result.stderr
    return optimized_ll


def _run_rockclimb_preprocess(
    tools,
    input_ll: Path,
    energy_config: Path,
    rockclimb_config: Path,
    output_ll: Path,
    max_unroll: int | None,
) -> subprocess.CompletedProcess[str]:
    return _run(
        [
            tools["opt"],
            "-load-pass-plugin",
            tools["pass_lib"],
            "-passes=rockclimb-preprocess",
            f"-energy-config={energy_config}",
            f"-rockclimb-config={rockclimb_config}",
            *(
                [f"-rockclimb-max-unroll-factor={max_unroll}"]
                if max_unroll is not None
                else []
            ),
            "-ckpt-log-level=info",
            "-S",
            str(input_ll),
            "-o",
            str(output_ll),
        ]
    )


def _count_ir_loads(ir_text: str) -> int:
    return ir_text.count("load i32")


def _parse_register_checkpoints(pass_output: str) -> int:
    for line in pass_output.splitlines():
        if "Register checkpoints:" not in line:
            continue
        return int(line.rsplit(":", 1)[1].strip())
    raise AssertionError("Could not find register checkpoint count in pass output")


def test_preprocess_partially_unrolls_constant_trip_loop(
    tools, compile_to_ir, tmp_path
):
    src = write_src(tmp_path, CONSTANT_LOOP)
    optimized_ll = _prepare_ir_for_preprocess(tools, compile_to_ir, src, tmp_path)
    output_ll = tmp_path / "preprocessed.ll"
    # Room for a few iterations but not all 8, so the budget picks K, not the
    # max-unroll cap.
    config_path = _write_rockclimb_config(
        tmp_path, capacity=3 * CONSTANT_LOOP_ITER_ENERGY + 4.0
    )

    before_ir = optimized_ll.read_text()
    result = _run_rockclimb_preprocess(
        tools,
        optimized_ll,
        IR_ENERGY_CONFIG,
        config_path,
        output_ll,
        None,
    )

    assert result.returncode == 0, result.stderr
    after_ir = output_ll.read_text()

    assert "RockClimbLoopUnrollPass: unrolled sum8::" in result.stdout + result.stderr
    assert _count_ir_loads(after_ir) > _count_ir_loads(before_ir)


def test_preprocess_caps_unroll_for_loop_that_fits_budget(
    tools, compile_to_ir, tmp_path
):
    src = write_src(tmp_path, CONSTANT_LOOP)
    optimized_ll = _prepare_ir_for_preprocess(tools, compile_to_ir, src, tmp_path)
    output_ll = tmp_path / "preprocessed.ll"
    # Room for the whole loop twice over, so only the max-unroll cap can limit K.
    config_path = _write_rockclimb_config(
        tmp_path, capacity=16 * CONSTANT_LOOP_ITER_ENERGY
    )

    before_ir = optimized_ll.read_text()
    result = _run_rockclimb_preprocess(
        tools,
        optimized_ll,
        IR_ENERGY_CONFIG,
        config_path,
        output_ll,
        None,
    )

    assert result.returncode == 0, result.stderr
    after_ir = output_ll.read_text()

    assert "RockClimbLoopUnrollPass: unrolled sum8::" in result.stdout + result.stderr
    assert _count_ir_loads(after_ir) > _count_ir_loads(before_ir)
    assert "br i1 %exitcond.not" in after_ir


def test_preprocess_honors_cli_max_unroll_factor(tools, compile_to_ir, tmp_path):
    src = write_src(tmp_path, CONSTANT_LOOP)
    optimized_ll = _prepare_ir_for_preprocess(tools, compile_to_ir, src, tmp_path)
    output_ll = tmp_path / "preprocessed.ll"
    # Ample budget, so the CLI flag is the only thing that can pick K.
    config_path = _write_rockclimb_config(
        tmp_path, capacity=16 * CONSTANT_LOOP_ITER_ENERGY
    )

    result = _run_rockclimb_preprocess(
        tools,
        optimized_ll,
        IR_ENERGY_CONFIG,
        config_path,
        output_ll,
        4,
    )

    assert result.returncode == 0, result.stderr
    assert "RockClimbLoopUnrollPass: unrolled sum8::" in result.stdout + result.stderr
    assert "K=4" in result.stdout + result.stderr


def test_preprocess_skips_unknown_trip_count_loop(tools, compile_to_ir, tmp_path):
    src = write_src(tmp_path, UNKNOWN_LOOP)
    optimized_ll = _prepare_ir_for_preprocess(tools, compile_to_ir, src, tmp_path)
    output_ll = tmp_path / "preprocessed.ll"
    config_path = _write_rockclimb_config(tmp_path, capacity=15.0)

    before_ir = optimized_ll.read_text()
    result = _run_rockclimb_preprocess(
        tools,
        optimized_ll,
        IR_ENERGY_CONFIG,
        config_path,
        output_ll,
        None,
    )

    assert result.returncode == 0, result.stderr
    assert "RockClimbLoopUnrollPass: unrolled" not in result.stdout + result.stderr
    assert _count_ir_loads(output_ll.read_text()) == _count_ir_loads(before_ir)


def test_compile_rockclimb_runs_preprocess(tmp_path, monkeypatch):
    env = ProjectEnv.from_environ(PROJECT_DIR)
    tc = Toolchain.resolve(env)

    src = write_src(tmp_path, CONSTANT_LOOP)
    config_path = _write_rockclimb_config(tmp_path, capacity=30.0)

    real_preprocess = rockclimb_module._run_rockclimb_preprocess
    seen: dict[str, bool] = {"called": False}

    def wrapped_preprocess(tc, env, opts, tmp, input_ll):
        seen["called"] = True
        return real_preprocess(tc, env, opts, tmp, input_ll)

    monkeypatch.setattr(
        rockclimb_module, "_run_rockclimb_preprocess", wrapped_preprocess
    )
    compile_rockclimb(
        tc,
        env,
        RockClimbCompileOptions(
            input_c=src,
            energy_config=ASSEMBLY_ENERGY_CONFIG,
            rockclimb_config=config_path,
            output=tmp_path / "rockclimb",
            pass_log_level="info",
            precomputed_energy=False,
            link=False,
            device_debug=False,
            halt_mode=None,
            cpu_freq=1_000_000,
            clang_opt_level=3,
            opt_level=3,
            max_unroll=None,
            save_temps=True,
            linker_script=None,
        ),
    )

    assert seen["called"] is True
    assert (tmp_path / "preprocessed.ll").exists()


def test_compile_rockclimb_recovers_inlining_from_o0_ir(tmp_path):
    env = ProjectEnv.from_environ(PROJECT_DIR)
    tc = Toolchain.resolve(env)

    src = write_src(tmp_path, HELPER_LOOP)
    config_path = _write_rockclimb_config(tmp_path, capacity=30.0)

    compile_rockclimb(
        tc,
        env,
        RockClimbCompileOptions(
            input_c=src,
            energy_config=ASSEMBLY_ENERGY_CONFIG,
            rockclimb_config=config_path,
            output=tmp_path / "rockclimb_inline",
            pass_log_level="info",
            precomputed_energy=False,
            link=False,
            device_debug=False,
            halt_mode=None,
            cpu_freq=1_000_000,
            clang_opt_level=3,
            opt_level=3,
            max_unroll=None,
            save_temps=True,
            linker_script=None,
        ),
    )

    optimized_ir = (tmp_path / "optimized.ll").read_text()
    assert "@add1" not in optimized_ir
    assert "noinline" not in optimized_ir


def test_compile_rockclimb_leaves_llvm_loop_unrolling_enabled(tmp_path, monkeypatch):
    env = ProjectEnv.from_environ(PROJECT_DIR)
    tc = Toolchain.resolve(env)

    src = write_src(tmp_path, CONSTANT_LOOP)
    config_path = _write_rockclimb_config(tmp_path, capacity=30.0)

    real_optimize = rockclimb_module.optimize_ir_with_options
    seen: dict[str, bool] = {}

    def wrapped_optimize_ir_with_options(
        tc, input_ll, output_ll, *, opt_level, disable_loop_unrolling
    ):
        seen["disable_loop_unrolling"] = disable_loop_unrolling
        return real_optimize(
            tc,
            input_ll,
            output_ll,
            opt_level=opt_level,
            disable_loop_unrolling=disable_loop_unrolling,
        )

    monkeypatch.setattr(
        rockclimb_module,
        "optimize_ir_with_options",
        wrapped_optimize_ir_with_options,
    )

    compile_rockclimb(
        tc,
        env,
        RockClimbCompileOptions(
            input_c=src,
            energy_config=ASSEMBLY_ENERGY_CONFIG,
            rockclimb_config=config_path,
            output=tmp_path / "rockclimb_unrolls",
            pass_log_level="info",
            precomputed_energy=False,
            link=False,
            device_debug=False,
            halt_mode=None,
            cpu_freq=1_000_000,
            clang_opt_level=3,
            opt_level=3,
            max_unroll=None,
            save_temps=False,
            linker_script=None,
        ),
    )

    assert seen["disable_loop_unrolling"] is False


def test_run_rockclimb_pass_skips_debug_marker_instrumentation(tmp_path, monkeypatch):
    env = ProjectEnv.from_environ(PROJECT_DIR)
    tc = Toolchain.resolve(env)
    config_path = _write_rockclimb_config(tmp_path, capacity=30.0)
    mir_file = tmp_path / "dummy.mir"
    mir_file.write_text("")

    seen: dict[str, list[str]] = {}

    def fake_run(cmd, *, check=True, step_name="", cwd=None, timeout=300, input=None):
        seen["cmd"] = cmd
        return StepResult(0, "", "", 0)

    monkeypatch.setattr(rockclimb_module, "run", fake_run)

    rockclimb_module._run_rockclimb_pass(
        tc,
        env,
        RockClimbCompileOptions(
            input_c=tmp_path / "dummy.c",
            energy_config=ASSEMBLY_ENERGY_CONFIG,
            rockclimb_config=config_path,
            output=tmp_path / "rockclimb_debug",
            pass_log_level="info",
            precomputed_energy=True,
            link=False,
            device_debug=True,
            halt_mode="swbor",
            cpu_freq=16_000_000,
            clang_opt_level=3,
            opt_level=3,
            max_unroll=None,
            save_temps=False,
            linker_script=None,
        ),
        tmp_path,
        mir_file,
        energy_flag=("-rockclimb-energy-data", str(tmp_path / "bb_energy.json")),
    )

    assert "-add-debug-markers" not in seen["cmd"]


def test_run_rockclimb_preprocess_passes_max_unroll_flag(tmp_path, monkeypatch):
    env = ProjectEnv.from_environ(PROJECT_DIR)
    tc = Toolchain.resolve(env)
    src = write_src(tmp_path, CONSTANT_LOOP)
    config_path = _write_rockclimb_config(tmp_path, capacity=30.0)

    seen: dict[str, list[str]] = {}
    real_preprocess = rockclimb_module._run_rockclimb_preprocess

    def wrapped_preprocess(tc, env, opts, tmp, input_ll):
        seen["max_unroll"] = opts.max_unroll
        return real_preprocess(tc, env, opts, tmp, input_ll)

    monkeypatch.setattr(
        rockclimb_module, "_run_rockclimb_preprocess", wrapped_preprocess
    )

    compile_rockclimb(
        tc,
        env,
        RockClimbCompileOptions(
            input_c=src,
            energy_config=ASSEMBLY_ENERGY_CONFIG,
            rockclimb_config=config_path,
            output=tmp_path / "rockclimb_max_unroll",
            pass_log_level="info",
            precomputed_energy=False,
            link=False,
            device_debug=False,
            halt_mode=None,
            cpu_freq=1_000_000,
            clang_opt_level=3,
            opt_level=3,
            max_unroll=5,
            save_temps=False,
            linker_script=None,
        ),
    )

    assert seen["max_unroll"] == 5


def test_crc_unroll_does_not_explode_distributed_checkpoints(tmp_path):
    env = ProjectEnv.from_environ(PROJECT_DIR)
    tc = Toolchain.resolve(env)
    crc_config = _write_board_like_config(tmp_path, capacity=LARGE_CAP_CAPACITY)

    out4 = compile_rockclimb(
        tc,
        env,
        RockClimbCompileOptions(
            input_c=CRC_BENCHMARK,
            energy_config=ASSEMBLY_ENERGY_CONFIG,
            rockclimb_config=crc_config,
            output=tmp_path / "crc_u4",
            pass_log_level="info",
            precomputed_energy=True,
            link=False,
            device_debug=False,
            halt_mode=None,
            cpu_freq=16_000_000,
            clang_opt_level=3,
            opt_level=3,
            max_unroll=4,
            save_temps=False,
            linker_script=None,
        ),
    )
    out16 = compile_rockclimb(
        tc,
        env,
        RockClimbCompileOptions(
            input_c=CRC_BENCHMARK,
            energy_config=ASSEMBLY_ENERGY_CONFIG,
            rockclimb_config=crc_config,
            output=tmp_path / "crc_u16",
            pass_log_level="info",
            precomputed_energy=True,
            link=False,
            device_debug=False,
            halt_mode=None,
            cpu_freq=16_000_000,
            clang_opt_level=3,
            opt_level=3,
            max_unroll=16,
            save_temps=False,
            linker_script=None,
        ),
    )

    checkpoints4 = _parse_register_checkpoints(out4.pass_output)
    checkpoints16 = _parse_register_checkpoints(out16.pass_output)

    assert checkpoints16 <= checkpoints4 * 3, (
        f"Expected max-unroll=16 to avoid checkpoint explosion relative to "
        f"max-unroll=4, got {checkpoints4} vs {checkpoints16}"
    )


def test_compile_rockclimb_handles_full_unroll_in_nested_loops(tmp_path):
    env = ProjectEnv.from_environ(PROJECT_DIR)
    tc = Toolchain.resolve(env)
    aes_config = _write_board_like_config(tmp_path, capacity=SMALL_CAP_CAPACITY)

    result = compile_rockclimb(
        tc,
        env,
        RockClimbCompileOptions(
            input_c=AES_BENCHMARK,
            energy_config=ASSEMBLY_ENERGY_CONFIG,
            rockclimb_config=aes_config,
            output=tmp_path / "aes_u5",
            pass_log_level="info",
            precomputed_energy=True,
            link=False,
            device_debug=False,
            halt_mode=None,
            cpu_freq=16_000_000,
            clang_opt_level=3,
            opt_level=3,
            max_unroll=5,
            save_temps=False,
            linker_script=None,
        ),
    )

    # A nested loop whose whole trip count fits the budget is unrolled away
    # entirely; doing that used to crash the pass, which then walked a loop it
    # had just deleted.
    assert "Stack dump:" not in result.pass_output
    fully_unrolled = re.findall(
        r"RockClimbLoopUnrollPass: fully unrolled main::\S+ N=(\d+) K=(\d+)",
        result.pass_output,
    )
    assert fully_unrolled, result.pass_output
    assert all(n == k for n, k in fully_unrolled), fully_unrolled

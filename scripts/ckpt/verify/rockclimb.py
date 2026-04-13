"""Semantic correctness verification for RockClimb checkpoint insertion."""

from __future__ import annotations

from pathlib import Path

from ..compile.rockclimb import RockClimbCompileOptions, compile_rockclimb
from ..env import ProjectEnv
from ..toolchain import Toolchain
from .common import InstrumentedOutput, verify_algorithm

_NVM_SYMBOLS = [
    "__nvm_done",
    "__nvm_result",
    "cnt_boundary",
    "cnt_save_reg",
    "cnt_restore_reg",
]


def verify_rockclimb(
    env: ProjectEnv,
    tc: Toolchain,
    *,
    benchmarks: list[str] | None,
    caps: list[str] | None,
    halt_mode: str,
    energy_config: Path | None,
    cpu_freq: int,
    pass_log_level: str,
) -> bool:
    """Verify semantic correctness of RockClimb checkpoint insertion."""
    from ..bench.config import default_energy_config

    if energy_config is None:
        energy_config = default_energy_config(env, "rockclimb")

    def compile_instrumented(
        tc: Toolchain,
        env: ProjectEnv,
        bench_path: Path,
        workdir: Path,
        cap_config: Path,
        halt_mode: str,
        cpu_freq: int,
    ) -> InstrumentedOutput:
        result = compile_rockclimb(
            tc, env,
            RockClimbCompileOptions(
                input_c=bench_path,
                energy_config=energy_config,
                rockclimb_config=cap_config,
                output=workdir / "rockclimb",
                pass_log_level=pass_log_level,
                precomputed_energy=True,
                link=True,
                device_debug=True,
                halt_mode=halt_mode,
                cpu_freq=cpu_freq,
                clang_opt_level=3,
                opt_level=3,
                max_unroll=None,
            ),
        )
        return InstrumentedOutput(
            compile_output=result.pass_output,
            elf_file=result.elf_file,
        )

    return verify_algorithm(
        env, tc,
        algorithm="rockclimb",
        benchmarks=benchmarks,
        caps=caps,
        halt_mode=halt_mode,
        cpu_freq=cpu_freq,
        nvm_symbols=_NVM_SYMBOLS,
        compile_instrumented=compile_instrumented,
    )

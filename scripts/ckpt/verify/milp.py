"""Semantic correctness verification for MILP checkpoint insertion."""

from __future__ import annotations

from pathlib import Path

from ..compile.milp import MilpCompileOptions, compile_milp
from ..env import ProjectEnv
from ..toolchain import Toolchain
from .common import InstrumentedOutput, verify_algorithm

_NVM_SYMBOLS = [
    "__nvm_done",
    "__nvm_result",
    "cnt_boundary",
    "cnt_save_vreg",
    "cnt_restore_vreg",
    "cnt_store_mem",
    "cnt_restore_mem",
]


def verify_milp(
    env: ProjectEnv,
    tc: Toolchain,
    *,
    benchmarks: list[str] | None,
    caps: list[str] | None,
    halt_mode: str,
    energy_config: Path | None,
    estimator_mode: str,
    cpu_freq: int,
    pass_log_level: str,
) -> bool:
    """Verify semantic correctness of MILP checkpoint insertion."""
    from ..bench.config import default_energy_config

    if energy_config is None:
        energy_config = default_energy_config(env, "milp")

    def compile_instrumented(
        tc: Toolchain,
        env: ProjectEnv,
        bench_path: Path,
        workdir: Path,
        cap_config: Path,
        halt_mode: str,
        cpu_freq: int,
    ) -> InstrumentedOutput:
        result = compile_milp(
            tc, env,
            MilpCompileOptions(
                input_c=bench_path,
                energy_config=energy_config,
                milp_config=cap_config,
                output=workdir / "milp",
                estimator_mode=estimator_mode,
                pass_log_level=pass_log_level,
                debug=False,
                link=True,
                device_debug=True,
                halt_mode=halt_mode,
                cpu_freq=cpu_freq,
                opt_level=3,
                clang_opt_level=3,
                milp_gap=0.05,
                milp_log_file="",
            ),
        )
        return InstrumentedOutput(
            compile_output=result.pass_output,
            elf_file=result.elf_file,
        )

    return verify_algorithm(
        env, tc,
        algorithm="milp",
        benchmarks=benchmarks,
        caps=caps,
        halt_mode=halt_mode,
        cpu_freq=cpu_freq,
        nvm_symbols=_NVM_SYMBOLS,
        compile_instrumented=compile_instrumented,
    )

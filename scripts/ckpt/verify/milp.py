"""Semantic correctness verification for MILP checkpoint insertion."""

from __future__ import annotations

from pathlib import Path

from ..bench.milp import NVM_SYMBOLS
from ..compile.milp import MilpCompileOptions, compile_milp
from ..env import ProjectEnv
from ..toolchain import Toolchain
from .common import (
    AlgorithmSpec,
    BenchResult,
    InstrumentedOutput,
    verify_algorithms,
)


def milp_spec(
    env: ProjectEnv,
    *,
    energy_config: Path | None,
    estimator_mode: str,
    coarse_allocation: bool,
    tripcount_annotations: bool,
    pass_log_level: str,
) -> AlgorithmSpec:
    """Build the MILP verification spec."""
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
        extra_defines: list[str],
    ) -> InstrumentedOutput:
        result = compile_milp(
            tc,
            env,
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
                milp_gap=0.0,
                milp_log_file="",
                coarse_allocation=coarse_allocation,
                tripcount_annotations=tripcount_annotations,
                save_temps=False,
                extra_defines=extra_defines,
            ),
        )
        return InstrumentedOutput(
            compile_output=result.pass_output,
            elf_file=result.elf_file,
        )

    return AlgorithmSpec(
        name="milp",
        nvm_symbols=NVM_SYMBOLS,
        compile_instrumented=compile_instrumented,
    )


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
    capture_timeout_seconds: float,
    coarse_allocation: bool,
    tripcount_annotations: bool,
    pass_log_level: str,
) -> list[BenchResult]:
    """Verify semantic correctness of MILP checkpoint insertion."""
    spec = milp_spec(
        env,
        energy_config=energy_config,
        estimator_mode=estimator_mode,
        coarse_allocation=coarse_allocation,
        tripcount_annotations=tripcount_annotations,
        pass_log_level=pass_log_level,
    )
    return verify_algorithms(
        env,
        tc,
        algorithms=[spec],
        benchmarks=benchmarks,
        caps=caps,
        halt_mode=halt_mode,
        cpu_freq=cpu_freq,
        capture_timeout_seconds=capture_timeout_seconds,
    )[spec.name]

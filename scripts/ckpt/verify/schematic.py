"""Semantic correctness verification for SCHEMATIC checkpoint insertion."""

from __future__ import annotations

from pathlib import Path

from ..bench.schematic import NVM_SYMBOLS
from ..compile.schematic import SchematicCompileOptions, compile_schematic
from ..env import ProjectEnv
from ..toolchain import Toolchain
from .common import (
    AlgorithmSpec,
    BenchResult,
    InstrumentedOutput,
    verify_algorithms,
)


def schematic_spec(
    env: ProjectEnv,
    *,
    energy_config: Path | None,
    estimator_mode: str,
    clang_opt_level: int,
    pass_log_level: str,
    algorithm_label: str,
    force_checkpoint_on_incompatible_loops: bool,
    recompute_energy_after_new_checkpoint: bool,
) -> AlgorithmSpec:
    """Build the SCHEMATIC verification spec."""
    from ..bench.config import default_energy_config

    if energy_config is None:
        energy_config = default_energy_config(env, algorithm_label)

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
        result = compile_schematic(
            tc,
            env,
            SchematicCompileOptions(
                input_c=bench_path,
                energy_config=energy_config,
                schematic_config=cap_config,
                output=workdir / "schematic",
                estimator_mode=estimator_mode,
                pass_log_level=pass_log_level,
                debug=False,
                trace_only=False,
                link=True,
                device_debug=True,
                halt_mode=halt_mode,
                cpu_freq=cpu_freq,
                opt_level=3,
                clang_opt_level=clang_opt_level,
                force_checkpoint_on_incompatible_loops=force_checkpoint_on_incompatible_loops,
                recompute_energy_after_new_checkpoint=recompute_energy_after_new_checkpoint,
                save_temps=False,
                trace_file=None,
                linker_script=None,
                extra_defines=extra_defines,
            ),
        )
        return InstrumentedOutput(
            compile_output=result.pass_output,
            elf_file=result.elf_file,
        )

    return AlgorithmSpec(
        name=algorithm_label,
        nvm_symbols=NVM_SYMBOLS,
        compile_instrumented=compile_instrumented,
    )


def verify_schematic(
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
    clang_opt_level: int,
    pass_log_level: str,
    algorithm_label: str,
    force_checkpoint_on_incompatible_loops: bool,
    recompute_energy_after_new_checkpoint: bool,
) -> list[BenchResult]:
    """Verify semantic correctness of SCHEMATIC checkpoint insertion."""
    spec = schematic_spec(
        env,
        energy_config=energy_config,
        estimator_mode=estimator_mode,
        clang_opt_level=clang_opt_level,
        pass_log_level=pass_log_level,
        algorithm_label=algorithm_label,
        force_checkpoint_on_incompatible_loops=force_checkpoint_on_incompatible_loops,
        recompute_energy_after_new_checkpoint=recompute_energy_after_new_checkpoint,
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

"""Semantic correctness verification for SCHEMATIC checkpoint insertion."""

from __future__ import annotations

from pathlib import Path

from ..bench.schematic import NVM_SYMBOLS
from ..compile.schematic import SchematicCompileOptions, compile_schematic
from ..env import ProjectEnv
from ..toolchain import Toolchain
from .common import InstrumentedOutput, verify_algorithm


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
) -> bool:
    """Verify semantic correctness of SCHEMATIC checkpoint insertion."""
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
            ),
        )
        return InstrumentedOutput(
            compile_output=result.pass_output,
            elf_file=result.elf_file,
        )

    return verify_algorithm(
        env,
        tc,
        algorithm=algorithm_label,
        benchmarks=benchmarks,
        caps=caps,
        halt_mode=halt_mode,
        cpu_freq=cpu_freq,
        capture_timeout_seconds=capture_timeout_seconds,
        nvm_symbols=NVM_SYMBOLS,
        compile_instrumented=compile_instrumented,
    )

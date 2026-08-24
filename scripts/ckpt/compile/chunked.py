"""Chunking-only compilation pipeline — loop strip-mining without instrumentation.

Runs the MILP preprocessing phases (loop canonicalization + strip-mining +
K re-clamp) and then compiles the chunked IR like the uninstrumented
baseline, skipping MILP solving and checkpoint instrumentation. Used to
isolate the control-flow overhead of loop chunking.
"""

from __future__ import annotations

import shutil
from dataclasses import dataclass, field
from pathlib import Path

from ..env import ProjectEnv
from ..runner import run
from ..tempdir import compilation_workdir
from ..toolchain import Toolchain
from .common import (
    compile_annotated_ir,
    compile_to_object,
    link_algorithm,
    optimize_ir,
    raises_compilation_error,
    run_assembly_energy,
    write_assembly_energy_config,
)

# (clang -O level, llc -O level) the chunked baseline is benchmarked at.
OPT_LEVELS = (3, 3)


@dataclass
class ChunkedCompileOptions:
    """Options for the chunking-only compilation pipeline."""

    input_c: Path
    energy_config: Path
    milp_config: Path
    output: Path
    pass_log_level: str
    device_debug: bool
    cpu_freq: int
    opt_level: int
    clang_opt_level: int
    link: bool
    extra_includes: list[str] = field(default_factory=list)
    extra_defines: list[str] = field(default_factory=list)


@dataclass
class ChunkedCompileResult:
    """Result of a chunking-only compilation."""

    object_file: Path
    assembly_file: Path
    elf_file: Path | None


@raises_compilation_error
def compile_chunked(
    tc: Toolchain,
    env: ProjectEnv,
    opts: ChunkedCompileOptions,
) -> ChunkedCompileResult:
    """Compile with loop chunking but without checkpoint insertion.

    Pipeline (the MILP assembly-mode pipeline up to and including the
    reclamp phase, then straight to codegen):
      compile_to_ir (raw -O3 frontend) -> tripcount annotation -> optimize_ir ->
      pre-strip-mining assembly energy -> milp-preprocess ->
      post-strip-mining assembly energy -> milp-reclamp-only ->
      compile to object -> link with uninstrumented boot.S + runtime
    """
    link = opts.link
    if opts.device_debug:
        link = True

    opts.output.parent.mkdir(parents=True, exist_ok=True)

    with compilation_workdir(prefix="ckpt_chunked_") as tmp:
        tripcount_ll = compile_annotated_ir(
            tc,
            env,
            input_c=opts.input_c,
            tmp=tmp,
            debug=False,
            device_debug=opts.device_debug,
            cpu_freq=opts.cpu_freq,
            extra_includes=opts.extra_includes,
            extra_defines=opts.extra_defines,
            tripcount_annotations=True,
        )

        # Middle-end optimization
        chunk_input_ll = tmp / "input_optimized.ll"
        optimize_ir(tc, tripcount_ll, chunk_input_ll, opt_level=opts.clang_opt_level)

        # Phase 2: Pre-strip-mining assembly energy
        pre_bb_energy, _ = run_assembly_energy(
            tc,
            env,
            chunk_input_ll,
            tmp / "pre",
            opts.energy_config,
            opts.pass_log_level,
            opt_level=opts.opt_level,
            stack_access_penalty=0.0,
        )
        pre_energy_config = write_assembly_energy_config(
            tmp / "pre_energy_config.json",
            pre_bb_energy,
        )

        # Phase 3: Preprocessing (loop canonicalization + strip-mining)
        preprocessed_ll = tmp / "preprocessed.ll"
        run(
            [
                tc.opt,
                f"-load-pass-plugin={env.pass_lib}",
                "-passes=milp-preprocess",
                f"-energy-config={pre_energy_config}",
                f"-milp-config={opts.milp_config}",
                f"-ckpt-log-level={opts.pass_log_level}",
                f"-loop-strip-mining-stats-json={tmp / 'strip_mining_stats.json'}",
                "-S",
                str(chunk_input_ll),
                "-o",
                str(preprocessed_ll),
            ],
            step_name="milp-preprocess",
        )

        # Phase 4: Post-strip-mining assembly energy
        post_bb_energy, _ = run_assembly_energy(
            tc,
            env,
            preprocessed_ll,
            tmp / "post",
            opts.energy_config,
            opts.pass_log_level,
            opt_level=opts.opt_level,
            stack_access_penalty=0.0,
        )
        post_energy_config = write_assembly_energy_config(
            tmp / "post_energy_config.json",
            post_bb_energy,
        )

        # Phase 4b: Re-clamp chunked strip-mined loops using post-strip-mining
        # energy so the chunked binary uses the same K as the MILP build.
        reclamped_ll = tmp / "reclamped.ll"
        run(
            [
                tc.opt,
                f"-load-pass-plugin={env.pass_lib}",
                "-passes=milp-reclamp-only",
                f"-energy-config={post_energy_config}",
                f"-milp-config={opts.milp_config}",
                f"-ckpt-log-level={opts.pass_log_level}",
                f"-loop-strip-mining-stats-json={tmp / 'strip_mining_reclamp_stats.json'}",
                "-S",
                str(preprocessed_ll),
                "-o",
                str(reclamped_ll),
            ],
            step_name="milp-reclamp-only",
        )

        # Compile chunked IR to MSP430 object (no MILP, no instrumentation)
        out_s = tmp / "chunked.s"
        out_o = tmp / "chunked.o"
        compile_to_object(tc, env, reclamped_ll, out_s, out_o, opt_level=opts.opt_level)

        shutil.copy2(out_o, opts.output.with_suffix(".o"))
        shutil.copy2(out_s, opts.output.with_suffix(".s"))

    # Link with the uninstrumented boot/runtime (no checkpoint runtime needed)
    elf_file: Path | None = None
    if link:
        elf_file = link_algorithm(
            tc,
            env,
            main_object=opts.output.with_suffix(".o"),
            output_elf=opts.output.with_suffix(".elf"),
            boot_source=env.uninstrumented_boot,
            runtime_source=env.uninstrumented_runtime,
            linker_script=env.milp_linker,
            boot_defines=[f"F_CPU={opts.cpu_freq}"],
            device_debug=opts.device_debug,
            cpu_freq=opts.cpu_freq,
            gcc_opt_level=opts.opt_level,
        )

    return ChunkedCompileResult(
        object_file=opts.output.with_suffix(".o"),
        assembly_file=opts.output.with_suffix(".s"),
        elf_file=elf_file,
    )

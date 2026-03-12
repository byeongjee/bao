"""MILP compilation pipeline — replaces compile_milp.sh.

Supports two estimator modes:
  - assembly (two-pass): pre/post strip-mining energy estimation via bb-energy-analyzer
  - ir (single-pass): IR-level energy estimation
"""

from __future__ import annotations

import shutil
import time
from dataclasses import dataclass, field
from pathlib import Path

from ..env import ProjectEnv
from ..runner import run
from ..tempdir import compilation_workdir
from ..toolchain import Toolchain
from .common import (
    annotate_tripcounts,
    assemble_and_link,
    assemble_boot,
    collect_bb_freq,
    compile_runtime_c,
    compile_to_ir,
    compile_to_object,
    optimize_ir,
    run_assembly_energy,
    write_assembly_energy_config,
)


@dataclass
class MilpCompileOptions:
    """Options for the MILP compilation pipeline."""

    input_c: Path
    energy_config: Path
    milp_config: Path
    output: Path
    estimator_mode: str
    verbose: bool
    debug: bool
    add_debug_markers: bool
    link: bool
    halt_mode: str
    debug_counters: bool
    opt_level: int = 2
    clang_opt_level: int = 2
    extra_includes: list[str] = field(default_factory=list)


@dataclass
class MilpCompileResult:
    """Result of a MILP compilation."""

    object_file: Path
    assembly_file: Path
    elf_file: Path | None
    pass_output: str
    profiling_time_ms: int


def compile_milp(
    tc: Toolchain,
    env: ProjectEnv,
    opts: MilpCompileOptions,
) -> MilpCompileResult:
    """Run the full MILP checkpoint insertion pipeline.

    Assembly mode (two-pass):
      compile_to_ir(-O0) -> tripcount annotation -> optimize_ir ->
      pre-strip-mining assembly energy -> milp-preprocess ->
      post-strip-mining assembly energy -> bb-freq-collect-only + collect_bb_freq ->
      milp-solve-only -> compile to object

    IR mode (single-pass):
      compile_to_ir(-O0) -> tripcount annotation -> optimize_ir ->
      bb-freq-collect -> collect_bb_freq -> milp pass -> compile to object

    Both modes optionally link with milp_boot.S + milp_runtime.c.
    """
    # bor/lpm4 halt modes and debug-counters imply linking
    link = opts.link
    if opts.halt_mode in ("bor", "lpm4"):
        link = True
    if opts.debug_counters:
        link = True

    opts.output.parent.mkdir(parents=True, exist_ok=True)

    with compilation_workdir(prefix="ckpt_milp_") as tmp:
        # Phase 1: C -> LLVM IR at -O0 (preserves loops for tripcount)
        input_ll = tmp / "input.ll"
        extra_includes = list(opts.extra_includes)
        extra_includes.append(str(env.project_dir / "passes" / "runtime"))

        compile_to_ir(
            tc, env, opts.input_c, input_ll,
            clang_opt_level=0,
            debug=opts.debug,
            debug_counters=opts.debug_counters,
            extra_includes=extra_includes,
        )

        # Tripcount annotation (before optimization)
        tripcount_ll = tmp / "tripcount.ll"
        annotate_tripcounts(tc, env, input_ll, tripcount_ll)

        # Frontend optimization
        milp_input_ll = tripcount_ll
        if opts.clang_opt_level != 0:
            optimized_ll = tmp / "input_optimized.ll"
            optimize_ir(tc, tripcount_ll, optimized_ll, opt_level=opts.clang_opt_level)
            milp_input_ll = optimized_ll

        # Build extra flags for MILP passes
        milp_extra_flags: list[str] = []
        if opts.add_debug_markers:
            milp_extra_flags.append("-add-debug-markers")
        if opts.verbose:
            milp_extra_flags += ["-loop-strip-mining-verbose", "-abstract-cfg-verbose"]

        if opts.estimator_mode == "assembly":
            pass_output, profiling_ms = _assembly_mode(
                tc, env, opts, tmp, milp_input_ll, milp_extra_flags,
            )
        else:
            pass_output, profiling_ms = _ir_mode(
                tc, env, opts, tmp, milp_input_ll, milp_extra_flags,
            )

        # Compile to MSP430 object
        ckpt_ll = tmp / "ckpt.ll"
        out_s = tmp / "ckpt.s"
        out_o = tmp / "ckpt.o"
        compile_to_object(tc, env, ckpt_ll, out_s, out_o, opt_level=opts.opt_level)

        # Copy outputs
        shutil.copy2(out_o, opts.output.with_suffix(".o"))
        shutil.copy2(out_s, opts.output.with_suffix(".s"))

        # Optional: link
        elf_file: Path | None = None
        if link:
            elf_file = _link_milp(tc, env, opts)

    return MilpCompileResult(
        object_file=opts.output.with_suffix(".o"),
        assembly_file=opts.output.with_suffix(".s"),
        elf_file=elf_file,
        pass_output=pass_output,
        profiling_time_ms=profiling_ms,
    )


# ---------------------------------------------------------------------------
# Assembly-mode two-pass pipeline
# ---------------------------------------------------------------------------

def _assembly_mode(
    tc: Toolchain,
    env: ProjectEnv,
    opts: MilpCompileOptions,
    tmp: Path,
    milp_input_ll: Path,
    milp_extra_flags: list[str],
) -> tuple[str, int]:
    """Assembly-based two-pass energy estimation pipeline.

    Returns (pass_output, profiling_time_ms).
    """
    # Phase 2: Pre-strip-mining assembly energy
    run_assembly_energy(tc, env, milp_input_ll, tmp / "pre", opts.energy_config)

    pre_energy_config = write_assembly_energy_config(
        tmp / "pre_energy_config.json",
        tmp / "pre.bb_energy.json",
    )

    # Phase 3: Preprocessing (loop canonicalization + strip-mining)
    preprocessed_ll = tmp / "preprocessed.ll"
    preprocess_cmd: list[str] = [
        tc.opt,
        f"-load-pass-plugin={env.pass_lib}",
        "-passes=milp-preprocess",
        f"-energy-config={pre_energy_config}",
        f"-milp-config={opts.milp_config}",
    ]
    if opts.verbose:
        preprocess_cmd.append("-loop-strip-mining-verbose")
    preprocess_cmd += ["-S", str(milp_input_ll), "-o", str(preprocessed_ll)]

    run(preprocess_cmd, step_name="milp-preprocess")

    # Phase 4: Post-strip-mining assembly energy
    run_assembly_energy(tc, env, preprocessed_ll, tmp / "post", opts.energy_config)

    post_energy_config = write_assembly_energy_config(
        tmp / "post_energy_config.json",
        tmp / "post.bb_energy.json",
    )

    # Phase 5: BB frequency collection
    profile_start = _now_ms()

    freq_inst_ll = tmp / "freq_inst.ll"
    run(
        [
            tc.opt,
            f"-load-pass-plugin={env.pass_lib}",
            "-passes=bb-freq-collect-only",
            "-S", str(preprocessed_ll),
            "-o", str(freq_inst_ll),
        ],
        step_name="bb-freq-collect-only",
    )

    bb_freq_json = collect_bb_freq(tc, env, freq_inst_ll, tmp)

    profiling_ms = _now_ms() - profile_start

    # Phase 6: MILP solving (on preprocessed IR)
    pass_output = _run_milp_pass(
        tc, env, opts,
        pass_name="milp-solve-only",
        energy_config=post_energy_config,
        input_ll=preprocessed_ll,
        output_ll=tmp / "ckpt.ll",
        bb_freq_json=bb_freq_json,
        milp_extra_flags=milp_extra_flags,
    )

    return pass_output, profiling_ms


# ---------------------------------------------------------------------------
# IR-mode single-pass pipeline
# ---------------------------------------------------------------------------

def _ir_mode(
    tc: Toolchain,
    env: ProjectEnv,
    opts: MilpCompileOptions,
    tmp: Path,
    milp_input_ll: Path,
    milp_extra_flags: list[str],
) -> tuple[str, int]:
    """IR-based single-pass energy estimation pipeline.

    Returns (pass_output, profiling_time_ms).
    """
    # BB frequency collection
    profile_start = _now_ms()

    freq_inst_ll = tmp / "freq_inst.ll"
    run(
        [
            tc.opt,
            f"-load-pass-plugin={env.pass_lib}",
            "-passes=bb-freq-collect",
            f"-energy-config={opts.energy_config}",
            f"-milp-config={opts.milp_config}",
            "-S", str(milp_input_ll),
            "-o", str(freq_inst_ll),
        ],
        step_name="bb-freq-collect",
    )

    bb_freq_json = collect_bb_freq(tc, env, freq_inst_ll, tmp)

    profiling_ms = _now_ms() - profile_start

    # MILP pass
    pass_output = _run_milp_pass(
        tc, env, opts,
        pass_name="milp",
        energy_config=opts.energy_config,
        input_ll=milp_input_ll,
        output_ll=tmp / "ckpt.ll",
        bb_freq_json=bb_freq_json,
        milp_extra_flags=milp_extra_flags,
    )

    return pass_output, profiling_ms


# ---------------------------------------------------------------------------
# Shared MILP pass invocation
# ---------------------------------------------------------------------------

def _run_milp_pass(
    tc: Toolchain,
    env: ProjectEnv,
    opts: MilpCompileOptions,
    *,
    pass_name: str,
    energy_config: Path,
    input_ll: Path,
    output_ll: Path,
    bb_freq_json: Path,
    milp_extra_flags: list[str],
) -> str:
    """Run a MILP opt pass and return its captured output."""
    cmd: list[str] = [
        tc.opt,
        f"-load-pass-plugin={env.pass_lib}",
        f"-passes={pass_name}",
        f"-energy-config={energy_config}",
        f"-milp-config={opts.milp_config}",
        f"-bb-freq-file={bb_freq_json}",
    ]
    cmd += milp_extra_flags
    cmd += ["-S", str(input_ll), "-o", str(output_ll)]

    result = run(cmd, step_name=pass_name)
    return result.output


# ---------------------------------------------------------------------------
# MILP link step
# ---------------------------------------------------------------------------

def _link_milp(
    tc: Toolchain,
    env: ProjectEnv,
    opts: MilpCompileOptions,
) -> Path:
    """Assemble and link the MILP output with boot.S + runtime.c."""
    output = opts.output

    # Boot assembly flags
    boot_defines: list[str] = []
    if opts.halt_mode == "bor":
        boot_defines.append("MILP_HALT_BOR")
    elif opts.halt_mode == "lpm4":
        boot_defines.append("MILP_HALT_LPM4")
    if opts.debug_counters:
        boot_defines.append("DEBUG_COUNTERS")

    # Assemble boot.S
    boot_o = output.with_suffix(".boot.o")
    assemble_boot(tc, env, env.milp_boot, boot_o, extra_defines=boot_defines)

    # Compile runtime.c
    runtime_o = output.with_suffix(".runtime.o")
    compile_runtime_c(tc, env, env.milp_runtime, runtime_o)

    link_objs: list[Path] = [
        output.with_suffix(".o"),
        boot_o,
        runtime_o,
    ]

    # Debug counters
    if opts.debug_counters:
        debug_o = output.with_suffix(".debug_counters.o")
        compile_runtime_c(
            tc, env, env.milp_debug_counters, debug_o,
            extra_defines=["DEBUG_COUNTERS"],
        )
        link_objs.append(debug_o)

    # Link
    elf = output.with_suffix(".elf")
    assemble_and_link(
        tc, env, link_objs, elf,
        linker_script=env.milp_linker,
    )

    return elf


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _now_ms() -> int:
    return int(time.monotonic() * 1000)

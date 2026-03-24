"""MILP compilation pipeline — replaces compile_milp.sh.

Supports two estimator modes:
  - assembly (two-pass): pre/post strip-mining energy estimation via bb-energy-analyzer
  - ir (single-pass): IR-level energy estimation
"""

from __future__ import annotations

import shutil
from dataclasses import dataclass, field
from pathlib import Path

from ..env import ProjectEnv
from ..runner import CompilationError, run
from ..tempdir import compilation_workdir
from ..toolchain import Toolchain
from . import common
from .common import (
    annotate_tripcounts,
    collect_bb_freq,
    compile_to_ir,
    compile_to_object,
    link_algorithm,
    now_ms,
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
    pass_log_level: str
    debug: bool
    link: bool
    halt_mode: str
    device_debug: bool
    cpu_freq: int
    opt_level: int
    clang_opt_level: int
    milp_gap: float
    milp_log_file: str
    save_temps: bool = False
    extra_includes: list[str] = field(default_factory=list)


@dataclass
class MilpCompileResult:
    """Result of a MILP compilation."""

    object_file: Path
    assembly_file: Path
    elf_file: Path | None
    pass_output: str
    profiling_time_ms: int
    stats_json: Path | None


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
    # bor/lpm4/swbor halt modes and debug-counters imply linking
    link = opts.link
    if opts.halt_mode in ("bor", "lpm4", "swbor"):
        link = True
    if opts.device_debug:
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
            device_debug=opts.device_debug,
            extra_includes=extra_includes,
            extra_defines=[f"F_CPU={opts.cpu_freq}"],
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
        if opts.device_debug:
            milp_extra_flags.append("-add-debug-markers")
        milp_extra_flags.append(f"-ckpt-log-level={opts.pass_log_level}")
        milp_extra_flags.append(f"-milp-gap={opts.milp_gap}")
        if opts.milp_log_file:
            milp_extra_flags.append(f"-milp-log-file={opts.milp_log_file}")

        try:
            if opts.estimator_mode == "assembly":
                pass_output, profiling_ms = _assembly_mode(
                    tc, env, opts, tmp, milp_input_ll, milp_extra_flags,
                )
            else:
                pass_output, profiling_ms = _ir_mode(
                    tc, env, opts, tmp, milp_input_ll, milp_extra_flags,
                )
        except CompilationError:
            if opts.save_temps:
                common.save_temps(tmp, opts.output.parent)
            raise

        # Copy stats JSON if available
        stats_json: Path | None = None
        stats_json_src = tmp / "stats.json"
        if stats_json_src.is_file():
            stats_json_dst = opts.output.with_suffix(".stats.json")
            shutil.copy2(stats_json_src, stats_json_dst)
            stats_json = stats_json_dst

        # Compile to MSP430 object + optional link
        # Wrap post-pass steps so pass_output is preserved on failure.
        elf_file: Path | None = None
        try:
            ckpt_ll = tmp / "ckpt.ll"
            out_s = tmp / "ckpt.s"
            out_o = tmp / "ckpt.o"
            compile_to_object(tc, env, ckpt_ll, out_s, out_o, opt_level=opts.opt_level)

            if opts.save_temps:
                common.save_temps(tmp, opts.output.parent)

            shutil.copy2(out_o, opts.output.with_suffix(".o"))
            shutil.copy2(out_s, opts.output.with_suffix(".s"))

            tmp_out = env.project_dir / "tmp"
            tmp_out.mkdir(parents=True, exist_ok=True)
            shutil.copy2(out_o, tmp_out / (opts.output.stem + ".o"))
            shutil.copy2(out_s, tmp_out / (opts.output.stem + ".s"))
            shutil.copy2(ckpt_ll, tmp_out / (opts.output.stem + ".ll"))

            if link:
                elf_file = _link_milp(tc, env, opts)
        except CompilationError as exc:
            if opts.save_temps:
                common.save_temps(tmp, opts.output.parent)
            exc.pass_output = pass_output
            exc.stats_json = stats_json
            raise

    return MilpCompileResult(
        object_file=opts.output.with_suffix(".o"),
        assembly_file=opts.output.with_suffix(".s"),
        elf_file=elf_file,
        pass_output=pass_output,
        profiling_time_ms=profiling_ms,
        stats_json=stats_json,
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
    pre_bb_energy, pre_stderr = run_assembly_energy(tc, env, milp_input_ll, tmp / "pre", opts.energy_config, opts.pass_log_level)

    pre_energy_config = write_assembly_energy_config(
        tmp / "pre_energy_config.json",
        pre_bb_energy,
    )

    # Phase 3: Preprocessing (loop canonicalization + strip-mining)
    preprocessed_ll = tmp / "preprocessed.ll"
    preprocess_cmd: list[str] = [
        tc.opt,
        f"-load-pass-plugin={env.pass_lib}",
        "-passes=milp-preprocess",
        f"-energy-config={pre_energy_config}",
        f"-milp-config={opts.milp_config}",
        f"-ckpt-log-level={opts.pass_log_level}",
    ]
    preprocess_cmd += ["-S", str(milp_input_ll), "-o", str(preprocessed_ll)]

    run(preprocess_cmd, step_name="milp-preprocess")

    # Phase 4: Post-strip-mining assembly energy
    post_bb_energy, post_stderr = run_assembly_energy(tc, env, preprocessed_ll, tmp / "post", opts.energy_config, opts.pass_log_level)

    post_energy_config = write_assembly_energy_config(
        tmp / "post_energy_config.json",
        post_bb_energy,
    )

    # Phase 5: BB frequency collection
    profile_start = now_ms()

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

    profiling_ms = now_ms() - profile_start

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

    pass_output = pre_stderr + post_stderr + pass_output
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
    profile_start = now_ms()

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

    profiling_ms = now_ms() - profile_start

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
    cmd.append(f"-ckpt-stats-json={output_ll.parent / 'stats.json'}")
    cmd += ["-S", str(input_ll), "-o", str(output_ll)]

    result = run(cmd, step_name=pass_name, timeout=660)
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
    boot_defines: list[str] = [f"F_CPU={opts.cpu_freq}"]
    if opts.halt_mode == "bor":
        boot_defines.append("HALT_BOR")
    elif opts.halt_mode == "lpm4":
        boot_defines.append("HALT_LPM4")
    elif opts.halt_mode == "swbor":
        boot_defines.append("HALT_SWBOR")
    if opts.device_debug:
        boot_defines.append("DEVICE_DEBUG")

    return link_algorithm(
        tc, env,
        main_object=opts.output.with_suffix(".o"),
        output_elf=opts.output.with_suffix(".elf"),
        boot_source=env.milp_boot,
        runtime_source=env.milp_runtime,
        linker_script=env.milp_linker,
        boot_defines=boot_defines,
        device_debug=opts.device_debug,
        cpu_freq=opts.cpu_freq,
        gcc_opt_level=opts.opt_level,
    )

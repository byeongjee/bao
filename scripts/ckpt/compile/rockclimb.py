"""RockClimb compilation pipeline — replaces compile_rockclimb.sh.

Machine-level (post-regalloc) greedy checkpoint insertion via MIR pipeline.
"""

from __future__ import annotations

import re
import shutil
from dataclasses import dataclass
from pathlib import Path

from ..env import ProjectEnv
from ..errors import CompilationError, ToolError
from ..runner import run
from ..tempdir import compilation_workdir
from ..toolchain import Toolchain
from . import common
from .common import (
    compile_annotated_ir,
    link_algorithm,
    optimize_ir_with_options,
    run_assembly_energy,
    write_assembly_energy_config,
)


@dataclass
class RockClimbCompileOptions:
    """Options for the RockClimb compilation pipeline."""

    input_c: Path
    energy_config: Path
    rockclimb_config: Path
    output: Path
    pass_log_level: str
    precomputed_energy: bool
    link: bool
    device_debug: bool
    halt_mode: str | None
    cpu_freq: int
    clang_opt_level: int
    opt_level: int
    max_unroll: int | None
    save_temps: bool
    linker_script: Path | None


@dataclass
class RockClimbCompileResult:
    """Result of a RockClimb compilation."""

    assembly_file: Path
    elf_file: Path | None
    pass_output: str
    stats_json: Path | None


@common.raises_compilation_error
def compile_rockclimb(
    tc: Toolchain,
    env: ProjectEnv,
    opts: RockClimbCompileOptions,
) -> RockClimbCompileResult:
    """Run the RockClimb machine-level checkpoint insertion pipeline.

    Pipeline:
      C -> clang(-O3 -disable-llvm-passes) -> .ll -> tripcount-annotation ->
      optimize_ir(-O{clang_opt_level}) ->
      pre-rockclimb assembly energy -> rockclimb-preprocess ->
      (if precomputed: assign-bb-debuginfo -> llc to obj -> bb-energy-analyzer) ->
      llc -stop-after=virtregrewriter -> .mir ->
      llc -run-pass=rockclimb -> .mir ->
      llc -start-after=virtregrewriter -> .s ->
      strip .cfi_* ->
      (if link: assemble + link with rockclimb_boot.S + rockclimb_runtime.c)
    """
    # bor/lpm4/swbor and debug-counters imply linking
    link = opts.link
    if opts.halt_mode in ("bor", "lpm4", "swbor"):
        link = True
    if opts.device_debug:
        link = True

    opts.output.parent.mkdir(parents=True, exist_ok=True)

    with compilation_workdir(prefix="ckpt_rockclimb_") as tmp:
        output = opts.output

        # Step 1: C -> raw frontend IR -> tripcount annotation. No passes have
        # run yet, so markers still sit in their source loops; user-written
        # noinline attributes are preserved (clang -O>=1 adds no blanket
        # noinline).
        annotated_ll = compile_annotated_ir(
            tc,
            env,
            input_c=opts.input_c,
            tmp=tmp,
            raw_frontend=True,
            debug=False,
            device_debug=opts.device_debug,
            cpu_freq=opts.cpu_freq,
            extra_includes=[],
        )

        # Step 1c: Run the standard optimization pipeline (the second half of
        # what a plain clang -O{n} compile would do).
        rockclimb_input_ll = annotated_ll
        if opts.clang_opt_level != 0:
            optimized_ll = tmp / "optimized.ll"
            optimize_ir_with_options(
                tc,
                annotated_ll,
                optimized_ll,
                opt_level=opts.clang_opt_level,
                disable_loop_unrolling=False,
            )
            rockclimb_input_ll = optimized_ll

        # Step 1d: Preprocess loops for RockClimb's energy-budgeted unroll policy.
        preprocess_output = _run_rockclimb_preprocess(
            tc, env, opts, tmp, rockclimb_input_ll
        )
        preprocessed_ll = tmp / "preprocessed.ll"

        if opts.precomputed_energy:
            pass_output = preprocess_output + _precomputed_pipeline(
                tc, env, opts, tmp, preprocessed_ll
            )
        else:
            pass_output = preprocess_output + _mir_estimation_pipeline(
                tc, env, opts, tmp, preprocessed_ll
            )

        stats_json = common.copy_stats_json(tmp, opts.output)

        if opts.save_temps:
            common.save_temps(tmp, opts.output.parent)

        # Post-pass steps: MIR -> assembly -> optional link
        # Wrap so pass_output is preserved on failure.
        elf_file: Path | None = None
        try:
            instrumented_mir = tmp / "instrumented.mir"
            raw_s = tmp / "raw.s"
            run(
                [
                    tc.llc,
                    "-march=msp430",
                    "-start-after=virtregrewriter",
                    str(instrumented_mir),
                    "-o",
                    str(raw_s),
                ],
                step_name="llc-mir-to-asm",
            )

            # Workaround: strip .cfi_* directives (LLVM MSP430 backend bug)
            clean_s = tmp / "clean.s"
            _strip_cfi_directives(raw_s, clean_s)
            shutil.copy2(clean_s, output.with_suffix(".s"))

            if link:
                elf_file = _link_rockclimb(tc, env, opts)
        except ToolError as exc:
            err = CompilationError(exc.step, exc.result)
            err.pass_output = pass_output
            err.stats_json = stats_json
            raise err from exc

    return RockClimbCompileResult(
        assembly_file=output.with_suffix(".s"),
        elf_file=elf_file,
        pass_output=pass_output,
        stats_json=stats_json,
    )


# ---------------------------------------------------------------------------
# Pre-computed energy pipeline
# ---------------------------------------------------------------------------


def _precomputed_pipeline(
    tc: Toolchain,
    env: ProjectEnv,
    opts: RockClimbCompileOptions,
    tmp: Path,
    input_ll: Path,
) -> str:
    """Pipeline with assembly-based pre-computed BB energy at MIR granularity.

    Uses assign-mir-bb-debuginfo to map MIR BBs to assembly via DWARF,
    so bb-energy-analyzer produces energy at MIR BB granularity (not IR BB).

    Returns pass_output (including bb-energy-analyzer stderr).
    """
    # Step 2: Assign IR-level BB debug info (provides DISubprogram infrastructure)
    bbinfo_ll = tmp / "bbinfo.ll"
    bb_mapping = tmp / "bb_mapping.json"
    run(
        [
            tc.opt,
            f"-load-pass-plugin={env.bb_debuginfo_lib}",
            "-passes=assign-bb-debuginfo",
            f"-bb-mapping={bb_mapping}",
            "-S",
            str(input_ll),
            "-o",
            str(bbinfo_ll),
        ],
        step_name="assign-bb-debuginfo",
    )

    # Step 3a: IR -> MIR (stop after register allocation)
    mir_file = tmp / "pre.mir"
    run(
        [
            tc.llc,
            "-march=msp430",
            "-stop-after=virtregrewriter",
            str(bbinfo_ll),
            "-o",
            str(mir_file),
        ],
        step_name="llc-to-mir",
    )

    # Step 3b: Assign MIR-level BB debug info (overwrites DebugLoc per MIR BB)
    mir_bb_mapping = tmp / "mir_bb_mapping.json"
    labeled_mir = tmp / "labeled.mir"
    run(
        [
            tc.llc,
            "-march=msp430",
            f"-load={env.machine_pass_lib}",
            "-run-pass=assign-mir-bb-debuginfo",
            f"-mir-bb-mapping={mir_bb_mapping}",
            str(mir_file),
            "-o",
            str(labeled_mir),
        ],
        step_name="assign-mir-bb-debuginfo",
    )

    # Step 3c: Labeled MIR -> object file (for energy analysis)
    energy_obj = tmp / "energy.o"
    run(
        [
            tc.llc,
            "-march=msp430",
            "-start-after=virtregrewriter",
            "-filetype=obj",
            str(labeled_mir),
            "-o",
            str(energy_obj),
        ],
        step_name="llc-energy-obj",
    )

    # Step 3d: bb-energy-analyzer (using MIR BB mapping)
    bb_energy = tmp / "bb_energy.json"
    result = run(
        [
            str(env.bb_analyzer),
            "--energy-params",
            str(opts.energy_config),
            "--bb-mapping",
            str(mir_bb_mapping),
            f"-ckpt-log-level={opts.pass_log_level}",
            str(energy_obj),
        ],
        step_name="bb-energy-analyzer",
    )
    bb_energy.write_text(result.stdout)
    analyzer_stderr = result.stderr

    # Step 4: RockClimb machine pass with pre-computed MIR BB energy
    pass_output = _run_rockclimb_pass(
        tc,
        env,
        opts,
        tmp,
        mir_file,
        energy_flag=("-rockclimb-energy-data", str(bb_energy)),
    )
    return analyzer_stderr + pass_output


# ---------------------------------------------------------------------------
# MIR-level estimation pipeline (no bb-energy-analyzer)
# ---------------------------------------------------------------------------


def _mir_estimation_pipeline(
    tc: Toolchain,
    env: ProjectEnv,
    opts: RockClimbCompileOptions,
    tmp: Path,
    input_ll: Path,
) -> str:
    """Pipeline with MIR-level energy estimation (fallback).

    Returns pass_output.
    """
    # Step 2: IR -> MIR
    mir_file = tmp / "pre.mir"
    run(
        [
            tc.llc,
            "-march=msp430",
            "-stop-after=virtregrewriter",
            str(input_ll),
            "-o",
            str(mir_file),
        ],
        step_name="llc-to-mir",
    )

    # Step 3: RockClimb machine pass with MIR-level estimation
    pass_output = _run_rockclimb_pass(
        tc,
        env,
        opts,
        tmp,
        mir_file,
        energy_flag=("-rockclimb-energy-config", str(opts.energy_config)),
    )
    return pass_output


def _run_rockclimb_preprocess(
    tc: Toolchain,
    env: ProjectEnv,
    opts: RockClimbCompileOptions,
    tmp: Path,
    input_ll: Path,
) -> str:
    """Run RockClimb's IR preprocess pass after generating pre-pass energy."""
    pre_bb_energy, pre_stderr = run_assembly_energy(
        tc,
        env,
        input_ll,
        tmp / "preprocess",
        opts.energy_config,
        opts.pass_log_level,
        opt_level=opts.opt_level,
    )
    preprocess_energy_config = write_assembly_energy_config(
        tmp / "preprocess_energy_config.json",
        pre_bb_energy,
    )

    preprocessed_ll = tmp / "preprocessed.ll"
    result = run(
        [
            tc.opt,
            f"-load-pass-plugin={env.pass_lib}",
            "-passes=rockclimb-preprocess",
            f"-energy-config={preprocess_energy_config}",
            f"-rockclimb-config={opts.rockclimb_config}",
            *(
                [f"-rockclimb-max-unroll-factor={opts.max_unroll}"]
                if opts.max_unroll is not None
                else []
            ),
            f"-ckpt-log-level={opts.pass_log_level}",
            "-S",
            str(input_ll),
            "-o",
            str(preprocessed_ll),
        ],
        step_name="rockclimb-preprocess",
    )
    return pre_stderr + result.output


# ---------------------------------------------------------------------------
# Shared RockClimb pass invocation
# ---------------------------------------------------------------------------


def _run_rockclimb_pass(
    tc: Toolchain,
    env: ProjectEnv,
    opts: RockClimbCompileOptions,
    tmp: Path,
    mir_file: Path,
    *,
    energy_flag: tuple[str, str],
) -> str:
    """Run the RockClimb machine pass via llc and return captured output."""
    instrumented_mir = tmp / "instrumented.mir"

    cmd: list[str] = [
        tc.llc,
        "-march=msp430",
        f"-load={env.machine_pass_lib}",
        "-run-pass=rockclimb",
        f"-rockclimb-config={opts.rockclimb_config}",
        energy_flag[0] + "=" + energy_flag[1],
        f"-ckpt-log-level={opts.pass_log_level}",
        f"-ckpt-stats-json={instrumented_mir.parent / 'stats.json'}",
        str(mir_file),
        "-o",
        str(instrumented_mir),
    ]
    # Per-save debug counters are intentionally disabled for RockClimb.
    # The inline counter sequence after each distributed register save
    # bloats large benchmarks enough to overflow FRAM.
    #
    # if opts.device_debug:
    #     cmd.insert(-2, "-add-debug-markers")

    # We always capture; the caller can choose whether to display.
    result = run(cmd, step_name="rockclimb-pass")
    return result.output


# ---------------------------------------------------------------------------
# CFI stripping workaround
# ---------------------------------------------------------------------------

_CFI_RE = re.compile(r"^\s*\.cfi_")


def _strip_cfi_directives(input_s: Path, output_s: Path) -> None:
    """Strip .cfi_* directives from assembly (LLVM MSP430 backend workaround)."""
    lines = input_s.read_text().splitlines(keepends=True)
    output_s.write_text("".join(line for line in lines if not _CFI_RE.match(line)))


# ---------------------------------------------------------------------------
# RockClimb link step
# ---------------------------------------------------------------------------


def _link_rockclimb(
    tc: Toolchain,
    env: ProjectEnv,
    opts: RockClimbCompileOptions,
) -> Path:
    """Assemble and link the RockClimb output with boot.S + runtime.c."""
    output = opts.output

    # Assemble the checkpoint-instrumented assembly to object
    asm_o = output.with_suffix(".o")
    run(
        [
            tc.gcc,
            f"-mmcu={env.device}",
            "-msmall",
            "-c",
            str(output.with_suffix(".s")),
            "-o",
            str(asm_o),
        ],
        step_name="gcc-assemble",
    )

    boot_defines = common.build_boot_defines(
        cpu_freq=opts.cpu_freq,
        halt_mode=opts.halt_mode,
        device_debug=opts.device_debug,
    )

    return link_algorithm(
        tc,
        env,
        main_object=asm_o,
        output_elf=output.with_suffix(".elf"),
        boot_source=env.rockclimb_boot,
        runtime_source=env.rockclimb_runtime,
        linker_script=opts.linker_script or env.rockclimb_linker,
        boot_defines=boot_defines,
        device_debug=opts.device_debug,
        cpu_freq=opts.cpu_freq,
        gcc_opt_level=opts.opt_level,
    )

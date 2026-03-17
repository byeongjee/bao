"""RockClimb compilation pipeline — replaces compile_rockclimb.sh.

Machine-level (post-regalloc) greedy checkpoint insertion via MIR pipeline.
"""

from __future__ import annotations

import re
import shutil
from dataclasses import dataclass
from pathlib import Path

from ..env import ProjectEnv
from ..runner import CompilationError, run
from ..tempdir import compilation_workdir
from ..toolchain import Toolchain
from . import common
from .common import (
    annotate_tripcounts,
    compile_to_ir,
    link_algorithm,
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
    halt_mode: str
    cpu_freq: int
    clang_opt_level: int = 2
    save_temps: bool = False
    linker_script: Path | None = None


@dataclass
class RockClimbCompileResult:
    """Result of a RockClimb compilation."""

    assembly_file: Path
    elf_file: Path | None
    pass_output: str
    stats_json: Path | None


def compile_rockclimb(
    tc: Toolchain,
    env: ProjectEnv,
    opts: RockClimbCompileOptions,
) -> RockClimbCompileResult:
    """Run the RockClimb machine-level checkpoint insertion pipeline.

    Pipeline:
      C -> clang -> .ll -> tripcount-annotation ->
      (if precomputed: assign-bb-debuginfo -> llc to obj -> bb-energy-analyzer) ->
      llc -stop-after=virtregrewriter -> .mir ->
      llc -run-pass=rockclimb -> .mir ->
      llc -start-after=virtregrewriter -> .s ->
      strip .cfi_* ->
      (if link: assemble + link with rockclimb_boot.S + rockclimb_runtime.c)
    """
    # bor/lpm4 and debug-counters imply linking
    link = opts.link
    if opts.halt_mode in ("bor", "lpm4"):
        link = True
    if opts.device_debug:
        link = True

    opts.output.parent.mkdir(parents=True, exist_ok=True)

    with compilation_workdir(prefix="ckpt_rockclimb_") as tmp:
        output = opts.output

        # Step 1: C -> LLVM IR
        raw_ll = tmp / "raw.ll"
        clang_defines: list[str] = [f"F_CPU={opts.cpu_freq}"]
        if opts.device_debug:
            clang_defines.append("DEVICE_DEBUG")

        compile_to_ir(
            tc, env, opts.input_c, raw_ll,
            clang_opt_level=opts.clang_opt_level,
            debug=False,
            device_debug=opts.device_debug,
            extra_includes=[str(env.project_dir / "passes" / "runtime")],
            extra_defines=clang_defines,
        )

        # Step 1b: Tripcount annotation
        annotated_ll = tmp / "annotated.ll"
        annotate_tripcounts(tc, env, raw_ll, annotated_ll)

        if opts.precomputed_energy:
            pass_output = _precomputed_pipeline(tc, env, opts, tmp, annotated_ll)
        else:
            pass_output = _mir_estimation_pipeline(tc, env, opts, tmp, annotated_ll)

        # Copy stats JSON if available
        stats_json: Path | None = None
        stats_json_src = tmp / "stats.json"
        if stats_json_src.is_file():
            stats_json_dst = opts.output.with_suffix(".stats.json")
            shutil.copy2(stats_json_src, stats_json_dst)
            stats_json = stats_json_dst

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
                    tc.llc, "-march=msp430",
                    "-start-after=virtregrewriter",
                    str(instrumented_mir),
                    "-o", str(raw_s),
                ],
                step_name="llc-mir-to-asm",
            )

            # Workaround: strip .cfi_* directives (LLVM MSP430 backend bug)
            clean_s = tmp / "clean.s"
            _strip_cfi_directives(raw_s, clean_s)
            shutil.copy2(clean_s, output.with_suffix(".s"))

            if link:
                elf_file = _link_rockclimb(tc, env, opts)
        except CompilationError as exc:
            exc.pass_output = pass_output
            exc.stats_json = stats_json
            raise

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
    annotated_ll: Path,
) -> str:
    """Pipeline with assembly-based pre-computed BB energy.

    Returns pass_output (including bb-energy-analyzer stderr).
    """
    # Step 2: Assign BB debug info
    bbinfo_ll = tmp / "bbinfo.ll"
    bb_mapping = tmp / "bb_mapping.json"
    run(
        [
            tc.opt,
            f"-load-pass-plugin={env.bb_debuginfo_lib}",
            "-passes=assign-bb-debuginfo",
            f"-bb-mapping={bb_mapping}",
            "-S", str(annotated_ll),
            "-o", str(bbinfo_ll),
        ],
        step_name="assign-bb-debuginfo",
    )

    # Step 3a: IR -> MIR (stop after register allocation)
    mir_file = tmp / "pre.mir"
    run(
        [
            tc.llc, "-march=msp430",
            "-stop-after=virtregrewriter",
            str(bbinfo_ll),
            "-o", str(mir_file),
        ],
        step_name="llc-to-mir",
    )

    # Step 3b: IR -> object file (for energy analysis)
    energy_obj = tmp / "energy.o"
    run(
        [
            tc.llc, "-march=msp430",
            "-filetype=obj",
            str(bbinfo_ll),
            "-o", str(energy_obj),
        ],
        step_name="llc-energy-obj",
    )

    # Step 3c: bb-energy-analyzer
    bb_energy = tmp / "bb_energy.json"
    result = run(
        [
            str(env.bb_analyzer),
            "--energy-params", str(opts.energy_config),
            "--bb-mapping", str(bb_mapping),
            str(energy_obj),
        ],
        step_name="bb-energy-analyzer",
    )
    bb_energy.write_text(result.stdout)
    analyzer_stderr = result.stderr

    # Step 4: RockClimb machine pass with pre-computed energy
    pass_output = _run_rockclimb_pass(
        tc, env, opts, tmp, mir_file,
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
    annotated_ll: Path,
) -> str:
    """Pipeline with MIR-level energy estimation (fallback).

    Returns pass_output.
    """
    # Step 2: IR -> MIR
    mir_file = tmp / "pre.mir"
    run(
        [
            tc.llc, "-march=msp430",
            "-stop-after=virtregrewriter",
            str(annotated_ll),
            "-o", str(mir_file),
        ],
        step_name="llc-to-mir",
    )

    # Step 3: RockClimb machine pass with MIR-level estimation
    pass_output = _run_rockclimb_pass(
        tc, env, opts, tmp, mir_file,
        energy_flag=("-rockclimb-energy-config", str(opts.energy_config)),
    )
    return pass_output


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
        tc.llc, "-march=msp430",
        f"-load={env.machine_pass_lib}",
        "-run-pass=rockclimb",
        f"-rockclimb-config={opts.rockclimb_config}",
        energy_flag[0] + "=" + energy_flag[1],
        f"-ckpt-log-level={opts.pass_log_level}",
        f"-ckpt-stats-json={instrumented_mir.parent / 'stats.json'}",
        str(mir_file),
        "-o", str(instrumented_mir),
    ]
    if opts.device_debug:
        cmd.insert(-2, "-add-debug-markers")

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
            "-c", str(output.with_suffix(".s")),
            "-o", str(asm_o),
        ],
        step_name="gcc-assemble",
    )

    boot_defines: list[str] = [f"F_CPU={opts.cpu_freq}"]
    if opts.halt_mode == "bor":
        boot_defines.append("HALT_BOR")
    elif opts.halt_mode == "lpm4":
        boot_defines.append("HALT_LPM4")
    if opts.device_debug:
        boot_defines.append("DEVICE_DEBUG")

    return link_algorithm(
        tc, env,
        main_object=asm_o,
        output_elf=output.with_suffix(".elf"),
        boot_source=env.rockclimb_boot,
        runtime_source=env.rockclimb_runtime,
        linker_script=opts.linker_script or env.rockclimb_linker,
        boot_defines=boot_defines,
        device_debug=opts.device_debug,
        cpu_freq=opts.cpu_freq,
    )

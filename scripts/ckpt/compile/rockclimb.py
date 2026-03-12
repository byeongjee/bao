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
from .common import (
    annotate_tripcounts,
    assemble_and_link,
    assemble_boot,
    compile_runtime_c,
    compile_to_ir,
)


@dataclass
class RockClimbCompileOptions:
    """Options for the RockClimb compilation pipeline."""

    input_c: Path
    energy_config: Path
    rockclimb_config: Path
    output: Path
    precomputed_energy: bool
    verbose: bool
    link: bool
    debug_counters: bool
    halt_mode: str
    clang_opt_level: int = 2
    linker_script: Path | None = None


@dataclass
class RockClimbCompileResult:
    """Result of a RockClimb compilation."""

    assembly_file: Path
    elf_file: Path | None
    pass_output: str


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
    if opts.debug_counters:
        link = True

    opts.output.parent.mkdir(parents=True, exist_ok=True)

    with compilation_workdir(prefix="ckpt_rockclimb_") as tmp:
        output = opts.output

        # Step 1: C -> LLVM IR
        raw_ll = tmp / "raw.ll"
        clang_defines: list[str] = []
        if opts.debug_counters:
            clang_defines.append("DEBUG_COUNTERS")

        compile_to_ir(
            tc, env, opts.input_c, raw_ll,
            clang_opt_level=opts.clang_opt_level,
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
            raise

    return RockClimbCompileResult(
        assembly_file=output.with_suffix(".s"),
        elf_file=elf_file,
        pass_output=pass_output,
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

    Returns the pass output string.
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

    # Step 4: RockClimb machine pass with pre-computed energy
    return _run_rockclimb_pass(
        tc, env, opts, tmp, mir_file,
        energy_flag=("-rockclimb-energy-data", str(bb_energy)),
    )


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

    Returns the pass output string.
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
    return _run_rockclimb_pass(
        tc, env, opts, tmp, mir_file,
        energy_flag=("-rockclimb-energy-config", str(opts.energy_config)),
    )


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
        str(mir_file),
        "-o", str(instrumented_mir),
    ]

    # In non-verbose mode the bash script suppresses stderr (2>/dev/null).
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
    output_s.write_text("".join(l for l in lines if not _CFI_RE.match(l)))


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
    clean_s = output.with_suffix(".s")

    # Assemble the checkpoint-instrumented assembly to object
    asm_o = output.with_suffix(".o")
    run(
        [
            tc.gcc,
            f"-mmcu={env.device}",
            "-msmall",
            "-c", str(clean_s),
            "-o", str(asm_o),
        ],
        step_name="gcc-assemble",
    )

    # Linker script
    linker_script = opts.linker_script or env.rockclimb_linker

    # Boot assembly flags
    boot_defines: list[str] = []
    if opts.halt_mode == "bor":
        boot_defines.append("ROCKCLIMB_HALT_BOR")
    elif opts.halt_mode == "lpm4":
        boot_defines.append("ROCKCLIMB_HALT_LPM4")

    # Assemble boot.S
    boot_o = output.with_suffix(".boot.o")
    assemble_boot(tc, env, env.rockclimb_boot, boot_o, extra_defines=boot_defines)

    # Compile runtime.c
    runtime_o = output.with_suffix(".runtime.o")
    compile_runtime_c(tc, env, env.rockclimb_runtime, runtime_o)

    link_objs: list[Path] = [asm_o, boot_o, runtime_o]

    # Debug counters
    if opts.debug_counters:
        debug_o = output.with_suffix(".debug_counters.o")
        compile_runtime_c(
            tc, env, env.rockclimb_debug_counters, debug_o,
            extra_defines=["DEBUG_COUNTERS"],
        )
        link_objs.append(debug_o)

    # Link
    elf = output.with_suffix(".elf")
    assemble_and_link(
        tc, env, link_objs, elf,
        linker_script=linker_script,
    )

    return elf

"""SCHEMATIC compilation pipeline — replaces compile_schematic.sh.

Trace-based checkpoint insertion: collect an execution trace via native
profiling, then use the SCHEMATIC pass to insert checkpoints.
"""

from __future__ import annotations

import shutil
import time
from dataclasses import dataclass, field
from pathlib import Path

from ..env import ProjectEnv
from ..runner import CompilationError, StepResult, run
from ..tempdir import compilation_workdir
from ..toolchain import Toolchain
from .common import (
    annotate_tripcounts,
    assemble_and_link,
    assemble_boot,
    compile_runtime_c,
    compile_to_ir,
    compile_to_object,
    optimize_ir,
    run_assembly_energy,
    write_assembly_energy_config,
)


@dataclass
class SchematicCompileOptions:
    """Options for the SCHEMATIC compilation pipeline."""

    input_c: Path
    energy_config: Path
    schematic_config: Path | None  # None when trace_only
    output: Path
    estimator_mode: str
    verbose: bool
    debug: bool
    add_debug_markers: bool
    trace_only: bool
    link: bool
    debug_counters: bool
    halt_mode: str
    opt_level: int = 2
    clang_opt_level: int = 2
    extra_includes: list[str] = field(default_factory=list)
    trace_file: Path | None = None
    linker_script: Path | None = None


@dataclass
class SchematicCompileResult:
    """Result of a SCHEMATIC compilation."""

    object_file: Path | None
    assembly_file: Path | None
    elf_file: Path | None
    trace_file: Path | None
    pass_output: str
    profiling_time_ms: int


def compile_schematic(
    tc: Toolchain,
    env: ProjectEnv,
    opts: SchematicCompileOptions,
) -> SchematicCompileResult:
    """Run the SCHEMATIC checkpoint insertion pipeline.

    Pipeline:
      compile_to_ir(-O0) -> tripcount-annotation -> optimize_ir ->
      (if no trace file: trace-collect -> compile native -> run -> get trace.json) ->
      (if trace_only: copy trace, return) ->
      schematic pass -> compile to object ->
      (if link: assemble + link with schematic_boot.S + schematic_runtime.c)
    """
    opts.output.parent.mkdir(parents=True, exist_ok=True)

    with compilation_workdir(prefix="ckpt_schematic_") as tmp:
        # Phase 1: C -> LLVM IR at -O0
        input_ll = tmp / "input.ll"
        extra_includes = list(opts.extra_includes)

        compile_to_ir(
            tc, env, opts.input_c, input_ll,
            clang_opt_level=0,
            debug=opts.debug,
            debug_counters=opts.debug_counters,
            extra_includes=extra_includes,
        )

        # Tripcount annotation
        tripcount_ll = tmp / "tripcount.ll"
        annotate_tripcounts(tc, env, input_ll, tripcount_ll)

        # Frontend optimization
        schematic_input_ll = tripcount_ll
        if opts.clang_opt_level != 0:
            optimized_ll = tmp / "input_optimized.ll"
            optimize_ir(
                tc, tripcount_ll, optimized_ll,
                opt_level=opts.clang_opt_level,
            )
            schematic_input_ll = optimized_ll

        # Trace collection
        trace_json, profiling_ms = _collect_or_reuse_trace(
            tc, env, opts, tmp, schematic_input_ll,
        )

        # --trace-only: copy trace and return early
        if opts.trace_only:
            trace_out = opts.output.with_name(opts.output.stem + "_trace.json")
            shutil.copy2(trace_json, trace_out)
            return SchematicCompileResult(
                object_file=None,
                assembly_file=None,
                elf_file=None,
                trace_file=trace_out,
                pass_output="",
                profiling_time_ms=profiling_ms,
            )

        # Assembly energy estimation (single-pass, no strip-mining)
        energy_config = opts.energy_config
        if opts.estimator_mode == "assembly":
            run_assembly_energy(
                tc, env, schematic_input_ll, tmp / "asm", opts.energy_config,
            )
            energy_config = write_assembly_energy_config(
                tmp / "asm_energy_config.json",
                tmp / "asm.bb_energy.json",
            )

        # SCHEMATIC pass
        pass_output = _run_schematic_pass(
            tc, env, opts, tmp, schematic_input_ll, trace_json,
            energy_config=energy_config,
        )

        # Compile to MSP430 object + optional link
        # Wrap post-pass steps so pass_output is preserved on failure.
        elf_file: Path | None = None
        try:
            ckpt_ll = tmp / "ckpt.ll"
            out_s = tmp / "ckpt.s"
            out_o = tmp / "ckpt.o"
            compile_to_object(
                tc, env, ckpt_ll, out_s, out_o,
                opt_level=opts.opt_level,
            )

            shutil.copy2(out_o, opts.output.with_suffix(".o"))
            shutil.copy2(out_s, opts.output.with_suffix(".s"))

            if opts.link or opts.debug_counters:
                elf_file = _link_schematic(tc, env, opts, tmp)
        except CompilationError as exc:
            exc.pass_output = pass_output
            raise

    return SchematicCompileResult(
        object_file=opts.output.with_suffix(".o"),
        assembly_file=opts.output.with_suffix(".s"),
        elf_file=elf_file,
        trace_file=trace_json if opts.trace_file is None else opts.trace_file,
        pass_output=pass_output,
        profiling_time_ms=profiling_ms,
    )


# ---------------------------------------------------------------------------
# Trace collection
# ---------------------------------------------------------------------------

def _collect_or_reuse_trace(
    tc: Toolchain,
    env: ProjectEnv,
    opts: SchematicCompileOptions,
    tmp: Path,
    schematic_input_ll: Path,
) -> tuple[Path, int]:
    """Collect an execution trace or reuse a pre-existing one.

    Returns (trace_json_path, profiling_time_ms).
    """
    if opts.trace_file is not None:
        return opts.trace_file, 0

    profile_start = _now_ms()

    # Instrument for trace collection
    trace_inst_ll = tmp / "trace_inst.ll"
    run(
        [
            tc.opt,
            f"-load-pass-plugin={env.pass_lib}",
            "-passes=trace-collect",
            f"-energy-config={opts.energy_config}",
            "-S", str(schematic_input_ll),
            "-o", str(trace_inst_ll),
        ],
        step_name="trace-collect",
    )

    # Strip MSP430 target triple for native compilation
    native_ll = tmp / "trace_inst_native.ll"
    ir_text = trace_inst_ll.read_text()
    lines: list[str] = []
    for line in ir_text.splitlines(keepends=True):
        if line.startswith("target triple = "):
            lines.append("\n")
        elif line.startswith("target datalayout = "):
            lines.append("\n")
        else:
            lines.append(line)
    native_ll.write_text("".join(lines))

    # Compile native trace binary
    trace_bin = tmp / "trace_run"
    run(
        [
            tc.clang, "-O0",
            *env.sysroot_flags,
            str(native_ll),
            str(env.schematic_trace_runtime),
            "-o", str(trace_bin),
        ],
        step_name="trace-compile",
    )

    # Run the trace binary (cwd must be tmp so it writes trace there)
    run(
        [str(trace_bin)],
        check=False,
        step_name="trace-run",
        cwd=str(tmp),
    )

    trace_json = tmp / "schematic_trace.json"
    if not trace_json.exists():
        raise CompilationError(
            "trace-collect",
            StepResult(1, "", "schematic_trace.json was not produced", 0),
        )

    profiling_ms = _now_ms() - profile_start
    return trace_json, profiling_ms


# ---------------------------------------------------------------------------
# SCHEMATIC pass invocation
# ---------------------------------------------------------------------------

def _run_schematic_pass(
    tc: Toolchain,
    env: ProjectEnv,
    opts: SchematicCompileOptions,
    tmp: Path,
    input_ll: Path,
    trace_json: Path,
    *,
    energy_config: Path | None = None,
) -> str:
    """Run the SCHEMATIC opt pass and return its captured output."""
    cfg = energy_config or opts.energy_config
    cmd: list[str] = [
        tc.opt,
        f"-load-pass-plugin={env.pass_lib}",
        "-passes=tripcount-annotation,schematic",
        f"-energy-config={cfg}",
        f"-schematic-config={opts.schematic_config}",
        f"-schematic-trace={trace_json}",
    ]
    if opts.add_debug_markers:
        cmd.append("-add-debug-markers")
    cmd += ["-S", str(input_ll), "-o", str(tmp / "ckpt.ll")]

    result = run(cmd, step_name="schematic-pass")
    return result.output


# ---------------------------------------------------------------------------
# SCHEMATIC link step
# ---------------------------------------------------------------------------

def _link_schematic(
    tc: Toolchain,
    env: ProjectEnv,
    opts: SchematicCompileOptions,
    tmp: Path,
) -> Path:
    """Assemble and link the SCHEMATIC output with boot.S + runtime.c."""
    output = opts.output

    # Linker script
    linker_script = opts.linker_script or env.schematic_linker

    # Boot assembly flags
    boot_defines: list[str] = []
    if opts.debug_counters:
        boot_defines.append("DEBUG_COUNTERS")
    if opts.halt_mode in ("bor", "lpm4"):
        boot_defines.append("HALT_MODE")

    # Assemble boot.S
    boot_o = tmp / "boot.o"
    assemble_boot(tc, env, env.schematic_boot, boot_o, extra_defines=boot_defines)

    # Compile runtime.c
    runtime_o = tmp / "runtime.o"
    compile_runtime_c(tc, env, env.schematic_runtime, runtime_o)

    link_objs: list[Path] = [
        output.with_suffix(".o"),
        boot_o,
        runtime_o,
    ]

    # Debug counters
    if opts.debug_counters:
        debug_o = tmp / "debug_counters.o"
        compile_runtime_c(
            tc, env, env.schematic_debug_counters, debug_o,
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


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _now_ms() -> int:
    return int(time.monotonic() * 1000)

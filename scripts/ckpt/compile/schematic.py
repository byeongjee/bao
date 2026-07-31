"""SCHEMATIC compilation pipeline — replaces compile_schematic.sh.

Trace-based checkpoint insertion: collect an execution trace via native
profiling, then use the SCHEMATIC pass to insert checkpoints.
"""

from __future__ import annotations

import json
import shutil
from dataclasses import dataclass, field
from pathlib import Path

from ..env import ProjectEnv
from ..runner import CompilationError, StepResult, run
from ..tempdir import compilation_workdir
from ..toolchain import Toolchain
from . import common
from .common import (
    MATH_LINK_FLAGS,
    annotate_tripcounts,
    canonicalize_ir_for_native_profiling,
    compile_to_ir,
    compile_to_object,
    isolate_calls,
    link_algorithm,
    now_ms,
    optimize_ir,
    run_assembly_energy,
    strip_ir_for_native,
    write_assembly_energy_config,
    write_native_stubs,
)


@dataclass
class SchematicCompileOptions:
    """Options for the SCHEMATIC compilation pipeline."""

    input_c: Path
    energy_config: Path
    schematic_config: Path | None  # None when trace_only
    output: Path
    estimator_mode: str
    pass_log_level: str
    debug: bool
    trace_only: bool
    link: bool
    device_debug: bool
    halt_mode: str | None
    cpu_freq: int
    opt_level: int
    clang_opt_level: int
    force_checkpoint_on_incompatible_loops: bool
    recompute_energy_after_new_checkpoint: bool
    save_temps: bool = False
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
    stats_json: Path | None


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
        extra_includes.append(str(env.project_dir / "passes" / "runtime"))

        compile_to_ir(
            tc,
            env,
            opts.input_c,
            input_ll,
            clang_opt_level=0,
            debug=opts.debug,
            device_debug=opts.device_debug,
            extra_includes=extra_includes,
            extra_defines=[f"F_CPU={opts.cpu_freq}"],
        )

        # Tripcount annotation
        tripcount_ll = tmp / "tripcount.ll"
        annotate_tripcounts(tc, env, input_ll, tripcount_ll)

        # Isolate function calls so the inter-procedural SCHEMATIC pass can fold
        # each callee's summary onto its call sites (replaces full inlining). The
        # same isolated IR feeds trace collection, energy analysis, and the solve
        # pass. At -O0 the calls survive (no inliner has run); at -O>=1 the
        # optimize_ir step below re-inlines and strips the isolation metadata, so
        # the run degrades gracefully to the single-function path.
        isolated_ll = tmp / "isolated.ll"
        isolate_calls(tc, env, tripcount_ll, isolated_ll)

        # Frontend optimization
        schematic_input_ll = isolated_ll
        if opts.clang_opt_level != 0:
            optimized_ll = tmp / "input_optimized.ll"
            optimize_ir(
                tc,
                isolated_ll,
                optimized_ll,
                opt_level=opts.clang_opt_level,
            )
            schematic_input_ll = optimized_ll

        # Trace collection
        trace_json, profiling_ms = _collect_or_reuse_trace(
            tc,
            env,
            opts,
            tmp,
            schematic_input_ll,
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
                stats_json=None,
            )

        # Assembly energy estimation (single-pass, no strip-mining)
        energy_config = opts.energy_config
        analyzer_stderr = ""
        if opts.estimator_mode == "assembly":
            bb_energy, analyzer_stderr = run_assembly_energy(
                tc,
                env,
                schematic_input_ll,
                tmp / "asm",
                opts.energy_config,
                opts.pass_log_level,
                opt_level=opts.opt_level,
            )
            energy_config = write_assembly_energy_config(
                tmp / "asm_energy_config.json",
                bb_energy,
            )

        # SCHEMATIC pass
        pass_output = analyzer_stderr + _run_schematic_pass(
            tc,
            env,
            opts,
            tmp,
            schematic_input_ll,
            trace_json,
            energy_config=energy_config,
        )

        # Copy stats JSON if available
        stats_json: Path | None = None
        stats_json_src = tmp / "stats.json"
        if stats_json_src.is_file():
            stats_json_dst = opts.output.with_suffix(".stats.json")
            shutil.copy2(stats_json_src, stats_json_dst)
            stats_json = stats_json_dst

        if opts.save_temps:
            common.save_temps(tmp, opts.output.parent)

        # Compile to MSP430 object + optional link
        # Wrap post-pass steps so pass_output is preserved on failure.
        elf_file: Path | None = None
        try:
            ckpt_ll = tmp / "ckpt.ll"
            out_s = tmp / "ckpt.s"
            out_o = tmp / "ckpt.o"
            compile_to_object(
                tc,
                env,
                ckpt_ll,
                out_s,
                out_o,
                opt_level=opts.opt_level,
            )

            shutil.copy2(out_o, opts.output.with_suffix(".o"))
            shutil.copy2(out_s, opts.output.with_suffix(".s"))

            import uuid

            run_id = uuid.uuid4().hex[:8]
            tmp_out = env.project_dir / "tmp" / f"schematic_{opts.output.stem}_{run_id}"
            tmp_out.mkdir(parents=True, exist_ok=True)
            for src in sorted(tmp.iterdir()):
                if src.is_file():
                    shutil.copy2(src, tmp_out / src.name)

            if opts.link or opts.device_debug:
                elf_file = _link_schematic(tc, env, opts)
        except CompilationError as exc:
            exc.pass_output = pass_output
            exc.stats_json = stats_json
            raise

    return SchematicCompileResult(
        object_file=opts.output.with_suffix(".o"),
        assembly_file=opts.output.with_suffix(".s"),
        elf_file=elf_file,
        trace_file=trace_json if opts.trace_file is None else opts.trace_file,
        pass_output=pass_output,
        profiling_time_ms=profiling_ms,
        stats_json=stats_json,
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

    profile_start = now_ms()

    # Instrument for trace collection
    trace_inst_ll = tmp / "trace_inst.ll"
    run(
        [
            tc.opt,
            f"-load-pass-plugin={env.pass_lib}",
            "-passes=trace-collect",
            f"-energy-config={opts.energy_config}",
            f"-ckpt-log-level={opts.pass_log_level}",
            "-S",
            str(schematic_input_ll),
            "-o",
            str(trace_inst_ll),
        ],
        step_name="trace-collect",
    )

    # Strip MSP430 target info for native compilation
    native_prep_ll = tmp / "trace_inst_native_prep.ll"
    native_ll = tmp / "trace_inst_native.ll"
    stubs_c = tmp / "debug_stubs.c"
    canonicalize_ir_for_native_profiling(tc, trace_inst_ll, native_prep_ll)
    strip_ir_for_native(native_prep_ll, native_ll)
    write_native_stubs(stubs_c)

    # Compile native trace binary
    trace_bin = tmp / "trace_run"
    run(
        [
            tc.clang,
            "-O0",
            *env.sysroot_flags,
            str(native_ll),
            str(env.schematic_trace_runtime),
            str(stubs_c),
            *MATH_LINK_FLAGS,
            "-o",
            str(trace_bin),
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

    profiling_ms = now_ms() - profile_start
    return trace_json, profiling_ms


# ---------------------------------------------------------------------------
# SCHEMATIC pass invocation
# ---------------------------------------------------------------------------


def _resolve_schematic_config(
    config_path: Path, clang_opt_level: int, tmp: Path
) -> Path:
    """Resolve opt-level-dependent fields in the SCHEMATIC config.

    If the config contains ``loop_increment_cost_nvm_O0`` /
    ``loop_increment_cost_nvm_O3`` keys, select the appropriate value based on
    *clang_opt_level* and write a resolved config (with a single
    ``loop_increment_cost_nvm``) into *tmp*.  If only the legacy
    ``loop_increment_cost_nvm`` key is present, return the original path
    unchanged.
    """
    with open(config_path) as f:
        config = json.load(f)

    key_o0 = "loop_increment_cost_nvm_O0"
    key_o3 = "loop_increment_cost_nvm_O3"

    has_per_opt = key_o0 in config or key_o3 in config
    if not has_per_opt:
        return config_path

    if clang_opt_level == 0:
        if key_o0 not in config:
            raise CompilationError(
                "schematic-config",
                StepResult(1, "", f"Missing '{key_o0}' in {config_path}", 0),
            )
        resolved_value = config[key_o0]
    else:
        if key_o3 not in config:
            raise CompilationError(
                "schematic-config",
                StepResult(1, "", f"Missing '{key_o3}' in {config_path}", 0),
            )
        resolved_value = config[key_o3]

    config["loop_increment_cost_nvm"] = resolved_value
    config.pop(key_o0, None)
    config.pop(key_o3, None)

    resolved_path = tmp / "schematic_config_resolved.json"
    with open(resolved_path, "w") as f:
        json.dump(config, f, indent=2)
    return resolved_path


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
    assert opts.schematic_config is not None, "schematic_config required for pass"
    schematic_cfg = _resolve_schematic_config(
        opts.schematic_config,
        opts.clang_opt_level,
        tmp,
    )
    cmd: list[str] = [
        tc.opt,
        f"-load-pass-plugin={env.pass_lib}",
        "-passes=schematic",
        f"-energy-config={cfg}",
        f"-schematic-config={schematic_cfg}",
        f"-schematic-trace={trace_json}",
        f"-ckpt-log-level={opts.pass_log_level}",
    ]
    if opts.device_debug:
        cmd.append("-add-debug-markers")
    if opts.force_checkpoint_on_incompatible_loops:
        cmd.append("-force-checkpoint-on-incompatible-loops")
    if opts.recompute_energy_after_new_checkpoint:
        cmd.append("-recompute-energy-after-new-checkpoint")
    cmd.append(f"-ckpt-stats-json={tmp / 'stats.json'}")
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
) -> Path:
    """Assemble and link the SCHEMATIC output with boot.S + runtime.c."""
    boot_defines: list[str] = [f"F_CPU={opts.cpu_freq}"]
    if opts.device_debug:
        boot_defines.append("DEVICE_DEBUG")
    if opts.halt_mode == "bor":
        boot_defines.append("HALT_BOR")
    elif opts.halt_mode == "lpm4":
        boot_defines.append("HALT_LPM4")
    elif opts.halt_mode == "swbor":
        boot_defines.append("HALT_SWBOR")

    return link_algorithm(
        tc,
        env,
        main_object=opts.output.with_suffix(".o"),
        output_elf=opts.output.with_suffix(".elf"),
        boot_source=env.schematic_boot,
        runtime_source=env.schematic_runtime,
        linker_script=opts.linker_script or env.schematic_linker,
        boot_defines=boot_defines,
        device_debug=opts.device_debug,
        cpu_freq=opts.cpu_freq,
        gcc_opt_level=opts.opt_level,
    )

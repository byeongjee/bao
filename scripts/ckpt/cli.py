"""Click-based CLI for the ckpt package.

Provides subcommands for compilation, benchmarking, verification,
analysis, and device interaction.
"""

from __future__ import annotations

import logging
from pathlib import Path

import click

from .analysis.plot import ALGORITHMS, METRICS
from .errors import (
    CkptError,
    CompilationError,
    ConfigError,
    DeviceError,
    InfeasibleError,
    ToolNotFoundError,
)
from .log import python_to_cpp_log_level, setup_logging

logger = logging.getLogger(__name__)

# Exit code mapping for CkptError subclasses.
_EXIT_CODES: list[tuple[type[CkptError], int]] = [
    (ConfigError, 2),
    (ToolNotFoundError, 3),
    (CompilationError, 4),
    (InfeasibleError, 5),
    (DeviceError, 6),
]


class CkptGroup(click.Group):
    """Custom group that catches :class:`CkptError` and exits cleanly."""

    def invoke(self, ctx: click.Context) -> None:
        try:
            super().invoke(ctx)
        except CkptError as exc:
            code = 1
            for exc_type, exc_code in _EXIT_CODES:
                if isinstance(exc, exc_type):
                    code = exc_code
                    break
            click.secho(f"Error: {exc}", fg="red", err=True)
            ctx.exit(code)


# ---------------------------------------------------------------------------
# Root group
# ---------------------------------------------------------------------------

@click.group(cls=CkptGroup)
@click.option(
    "--log-level",
    type=click.Choice(
        ["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"],
        case_sensitive=False,
    ),
    default="INFO",
    help="Set logging level.",
)
@click.pass_context
def main(ctx: click.Context, log_level: str) -> None:
    """ckpt -- Checkpoint insertion toolchain."""
    from .env import ProjectEnv
    from .toolchain import Toolchain

    setup_logging(log_level)

    env = ProjectEnv.from_environ()
    tc = Toolchain.resolve(env)
    ctx.ensure_object(dict)
    ctx.obj["env"] = env
    ctx.obj["tc"] = tc
    ctx.obj["log_level"] = log_level.lower()
    ctx.obj["pass_log_level"] = python_to_cpp_log_level(log_level)


# =========================================================================
# compile group
# =========================================================================

@main.group()
def compile() -> None:
    """Compilation pipelines."""


def _resolve_input(env, input_c: str) -> Path:
    """Resolve INPUT_C as a path or benchmark name."""
    from .bench.config import discover_benchmarks

    p = Path(input_c)
    if p.is_file():
        return p

    # Try as benchmark name
    matches = discover_benchmarks(env, [input_c])
    if matches:
        return matches[0]

    raise click.UsageError(
        f"'{input_c}' is not a file and not found in benchmarks/intermittent/."
    )


def _resolve_algorithm_config(
    env,
    algorithm: str,
    explicit_config: str | None,
    cap: str | None,
    option_name: str,
) -> Path:
    """Resolve an algorithm config from explicit path or --cap."""
    from .bench.config import discover_capacitors

    if explicit_config is not None:
        return Path(explicit_config)

    if cap is not None:
        caps = discover_capacitors(env, algorithm, [cap])
        return caps[0].config_path

    raise click.UsageError(
        f"Provide {option_name} or --cap."
    )


@compile.command("milp")
@click.argument("input_c")
@click.option("-e", "--energy-config", type=click.Path(exists=True),
              help="Energy config (default: auto-detected by estimator mode).")
@click.option("-m", "--milp-config", type=click.Path(exists=True),
              help="MILP config (alternative to --cap).")
@click.option("--cap", help="Capacitor size (e.g. 1uF) — resolves to benchmarks/config_{cap}.json.")
@click.option("-o", "--output", type=click.Path())
@click.option("--link", is_flag=True, help="Link with boot.S and runtime.")
@click.option(
    "--estimator-mode",
    type=click.Choice(["assembly", "ir"]),
    default="assembly",
    help="Energy estimator mode.",
)
@click.option("--debug", is_flag=True, help="Enable DEBUG output.")
@click.option("--device-debug", is_flag=True, help="Enable device debug.")
@click.option(
    "--halt-mode",
    type=click.Choice(["nop", "bor", "lpm4", "swbor"]),
    default="nop",
    help="Halt mode for linked binary.",
)
@click.option("-O", "opt_level", type=int, default=3, help="LLC opt level.")
@click.option("-Oc", "clang_opt_level", type=int, default=3, help="Clang opt level.")
@click.option("-I", "extra_includes", multiple=True, help="Extra include dirs.")
@click.option(
    "--cpu-freq",
    type=click.Choice(["1", "8", "16"]),
    default="1",
    help="CPU frequency in MHz (default: 1).",
)
@click.option("--save-temps", is_flag=True, help="Save intermediate files to output directory.")
@click.option(
    "--milp-gap",
    type=float,
    default=0.0,
    help="MIP optimality gap (default: 0.0 = proven optimal).",
)
@click.option("--milp-log-file", type=click.Path(), default="", help="Gurobi log file path.")
@click.option("--coarse-allocation", is_flag=True,
              help="Use one MILP placement variable per eligible value instead of per-region placement.")
@click.option("--accumulate-keys", type=click.Path(), help="Accumulate required energy keys to this file.")
@click.pass_context
def compile_milp_cmd(
    ctx: click.Context,
    input_c: str,
    energy_config: str | None,
    milp_config: str | None,
    cap: str | None,
    output: str | None,
    link: bool,
    estimator_mode: str,
    debug: bool,
    device_debug: bool,
    halt_mode: str,
    opt_level: int,
    clang_opt_level: int,
    extra_includes: tuple[str, ...],
    cpu_freq: str,
    save_temps: bool,
    milp_gap: float,
    milp_log_file: str,
    coarse_allocation: bool,
    accumulate_keys: str | None,
) -> None:
    """Run the MILP checkpoint insertion compilation pipeline.

    INPUT_C can be a benchmark name (e.g. "crc") or a path to a C file.
    """
    from .bench.config import default_energy_config
    from .compile.milp import MilpCompileOptions, compile_milp

    env = ctx.obj["env"]

    input_path = _resolve_input(env, input_c)
    output_path = Path(output) if output else Path("build") / input_path.stem

    milp_config_path = _resolve_algorithm_config(
        env, "milp", milp_config, cap, "-m/--milp-config",
    )

    energy_config_path: Path
    if energy_config is not None:
        energy_config_path = Path(energy_config)
    elif estimator_mode == "ir":
        energy_config_path = env.project_dir / "benchmarks" / "sample_energy_config_ir.json"
    else:
        energy_config_path = default_energy_config(env, "milp")

    cpu_freq_hz = int(cpu_freq) * 1_000_000

    result = compile_milp(
        ctx.obj["tc"],
        env,
        MilpCompileOptions(
            input_c=input_path,
            energy_config=energy_config_path,
            milp_config=milp_config_path,
            output=output_path,
            opt_level=opt_level,
            clang_opt_level=clang_opt_level,
            extra_includes=list(extra_includes),
            pass_log_level=ctx.obj["pass_log_level"],
            debug=debug,
            link=link,
            halt_mode=halt_mode,
            device_debug=device_debug,
            estimator_mode=estimator_mode,
            cpu_freq=cpu_freq_hz,
            milp_gap=milp_gap,
            milp_log_file=milp_log_file,
            coarse_allocation=coarse_allocation,
            save_temps=save_temps,
        ),
    )

    if accumulate_keys:
        from .bench.runner import accumulate_keys_to_file, extract_energy_params
        ep = extract_energy_params(result.pass_output)
        if ep is not None:
            accumulate_keys_to_file(set(ep[0]), Path(accumulate_keys))

    logger.info("Object: %s", result.object_file)
    logger.info("Assembly: %s", result.assembly_file)
    if result.elf_file:
        logger.info("ELF: %s", result.elf_file)
    logger.debug("Pass output:\n%s", result.pass_output)


@compile.command("rockclimb")
@click.argument("input_c")
@click.option("-e", "--energy-config", type=click.Path(exists=True),
              help="Energy config (default: auto-detected).")
@click.option("-c", "--rockclimb-config", type=click.Path(exists=True),
              help="RockClimb config (alternative to --cap).")
@click.option("--cap", help="Capacitor size (e.g. 1uF) — resolves to benchmarks/config_{cap}.json.")
@click.option("-o", "--output", type=click.Path())
@click.option("--link", is_flag=True, help="Link with boot.S and runtime.")
@click.option("--device-debug", is_flag=True, help="Enable device debug.")
@click.option(
    "--halt-mode",
    type=click.Choice(["nop", "bor", "lpm4", "swbor"]),
    default="nop",
    help="Halt mode for linked binary.",
)
@click.option("-O", "opt_level", type=int, default=3, help="LLC opt level.")
@click.option("-Oc", "clang_opt_level", type=int, default=3, help="Clang opt level.")
@click.option(
    "--no-precomputed-energy",
    is_flag=True,
    help="Disable assembly-based pre-computed BB energy; use MIR-level estimation instead.",
)
@click.option(
    "--cpu-freq",
    type=click.Choice(["1", "8", "16"]),
    default="1",
    help="CPU frequency in MHz (default: 1).",
)
@click.option("--save-temps", is_flag=True, help="Save intermediate files to output directory.")
@click.option("--accumulate-keys", type=click.Path(), help="Accumulate required energy keys to this file.")
@click.pass_context
def compile_rockclimb_cmd(
    ctx: click.Context,
    input_c: str,
    energy_config: str | None,
    rockclimb_config: str | None,
    cap: str | None,
    output: str | None,
    link: bool,
    device_debug: bool,
    halt_mode: str,
    opt_level: int,
    clang_opt_level: int,
    no_precomputed_energy: bool,
    cpu_freq: str,
    save_temps: bool,
    accumulate_keys: str | None,
) -> None:
    """Run the RockClimb machine-level compilation pipeline.

    INPUT_C can be a benchmark name (e.g. "crc") or a path to a C file.
    """
    from .bench.config import default_energy_config
    from .compile.rockclimb import RockClimbCompileOptions, compile_rockclimb

    env = ctx.obj["env"]

    input_path = _resolve_input(env, input_c)
    output_path = Path(output) if output else Path("build") / input_path.stem

    rockclimb_config_path = _resolve_algorithm_config(
        env, "rockclimb", rockclimb_config, cap, "-c/--rockclimb-config",
    )

    energy_config_path: Path
    if energy_config is not None:
        energy_config_path = Path(energy_config)
    else:
        energy_config_path = default_energy_config(env, "rockclimb")

    cpu_freq_hz = int(cpu_freq) * 1_000_000

    result = compile_rockclimb(
        ctx.obj["tc"],
        env,
        RockClimbCompileOptions(
            input_c=input_path,
            energy_config=energy_config_path,
            rockclimb_config=rockclimb_config_path,
            output=output_path,
            pass_log_level=ctx.obj["pass_log_level"],
            clang_opt_level=clang_opt_level,
            opt_level=opt_level,
            precomputed_energy=not no_precomputed_energy,
            link=link,
            device_debug=device_debug,
            halt_mode=halt_mode,
            cpu_freq=cpu_freq_hz,
            save_temps=save_temps,
        ),
    )

    if accumulate_keys:
        import json as _json
        from .bench.runner import accumulate_keys_to_file, extract_energy_params
        ep = extract_energy_params(result.pass_output)
        if ep is not None:
            accumulate_keys_to_file(set(ep[0]), Path(accumulate_keys))
        elif result.stats_json is not None and result.stats_json.is_file():
            data = _json.loads(result.stats_json.read_text())
            req = data.get("required_energy_keys", [])
            if req:
                accumulate_keys_to_file(set(req), Path(accumulate_keys))

    logger.info("Assembly: %s", result.assembly_file)
    if result.elf_file:
        logger.info("ELF: %s", result.elf_file)
    logger.debug("Pass output:\n%s", result.pass_output)


def _compile_schematic_impl(
    ctx: click.Context,
    input_c: str,
    energy_config: str | None,
    schematic_config: str | None,
    cap: str | None,
    output: str | None,
    link: bool,
    debug: bool,
    device_debug: bool,
    halt_mode: str,
    trace_file: str | None,
    trace_only: bool,
    opt_level: int,
    clang_opt_level: int,
    extra_includes: tuple[str, ...],
    estimator_mode: str,
    cpu_freq: str,
    save_temps: bool,
    algorithm_label: str,
    accumulate_keys: str | None,
    force_checkpoint_on_incompatible_loops: bool,
    recompute_energy_after_new_checkpoint: bool,
) -> None:
    from .bench.config import default_energy_config
    from .compile.schematic import SchematicCompileOptions, compile_schematic

    env = ctx.obj["env"]

    input_path = _resolve_input(env, input_c)
    output_path = Path(output) if output else Path("build") / input_path.stem
    cpu_freq_hz = int(cpu_freq) * 1_000_000

    schematic_config_path: Path | None
    if not trace_only:
        schematic_config_path = _resolve_algorithm_config(
            env, algorithm_label, schematic_config, cap,
            "-s/--schematic-config or --cap",
        )
    else:
        schematic_config_path = Path(schematic_config) if schematic_config else None

    energy_config_path: Path
    if energy_config is not None:
        energy_config_path = Path(energy_config)
    elif estimator_mode == "ir":
        energy_config_path = env.project_dir / "benchmarks" / "sample_energy_config_ir.json"
    else:
        energy_config_path = default_energy_config(env, algorithm_label)

    result = compile_schematic(
        ctx.obj["tc"],
        env,
        SchematicCompileOptions(
            input_c=input_path,
            energy_config=energy_config_path,
            schematic_config=schematic_config_path,
            output=output_path,
            estimator_mode=estimator_mode,
            pass_log_level=ctx.obj["pass_log_level"],
            debug=debug,
            trace_only=trace_only,
            link=link,
            device_debug=device_debug,
            halt_mode=halt_mode,
            cpu_freq=cpu_freq_hz,
            opt_level=opt_level,
            clang_opt_level=clang_opt_level,
            extra_includes=list(extra_includes),
            trace_file=Path(trace_file) if trace_file else None,
            save_temps=save_temps,
            force_checkpoint_on_incompatible_loops=force_checkpoint_on_incompatible_loops,
            recompute_energy_after_new_checkpoint=recompute_energy_after_new_checkpoint,
        ),
    )

    if accumulate_keys:
        from .bench.runner import accumulate_keys_to_file, extract_energy_params
        ep = extract_energy_params(result.pass_output)
        if ep is not None:
            accumulate_keys_to_file(set(ep[0]), Path(accumulate_keys))

    if result.object_file:
        logger.info("Object: %s", result.object_file)
    if result.assembly_file:
        logger.info("Assembly: %s", result.assembly_file)
    if result.elf_file:
        logger.info("ELF: %s", result.elf_file)
    if result.trace_file:
        logger.info("Trace: %s", result.trace_file)
    if result.pass_output:
        logger.debug("Pass output:\n%s", result.pass_output)


@compile.command("schematic")
@click.argument("input_c")
@click.option("-e", "--energy-config", type=click.Path(exists=True),
              help="Energy config (default: auto-detected by estimator mode).")
@click.option("-s", "--schematic-config", type=click.Path(exists=True),
              help="SCHEMATIC config (alternative to --cap).")
@click.option("--cap", help="Capacitor size (e.g. 1uF) — resolves to benchmarks/config_{cap}.json.")
@click.option("-o", "--output", type=click.Path())
@click.option("--link", is_flag=True, help="Link with boot.S and runtime.")
@click.option("--debug", is_flag=True, help="Enable DEBUG output.")
@click.option("--device-debug", is_flag=True, help="Enable device debug.")
@click.option(
    "--halt-mode",
    type=click.Choice(["nop", "bor", "lpm4", "swbor"]),
    default="nop",
    help="Halt mode for linked binary.",
)
@click.option(
    "--trace-file", type=click.Path(exists=True), help="Pre-collected trace JSON."
)
@click.option("--trace-only", is_flag=True, help="Only collect trace, skip insertion.")
@click.option("-O", "opt_level", type=int, default=3, help="LLC opt level.")
@click.option("-Oc", "clang_opt_level", type=int, default=0, help="Clang opt level.")
@click.option("-I", "extra_includes", multiple=True, help="Extra include dirs.")
@click.option(
    "--estimator-mode",
    type=click.Choice(["assembly", "ir"]),
    default="assembly",
    help="Energy estimator mode.",
)
@click.option(
    "--cpu-freq",
    type=click.Choice(["1", "8", "16"]),
    default="1",
    help="CPU frequency in MHz (default: 1).",
)
@click.option("--save-temps", is_flag=True, help="Save intermediate files to output directory.")
@click.option("--accumulate-keys", type=click.Path(), help="Accumulate required energy keys to this file.")
@click.option("--force-checkpoint-on-incompatible-loops", is_flag=True,
              help="Force checkpoint at loop header when inner loop allocations conflict.")
@click.option("--recompute-energy-after-new-checkpoint", is_flag=True,
              help="Recompute local E_left/E_to_leave after inserting a new checkpoint (disabled by default; deviates from the reference implementation).")
@click.pass_context
def compile_schematic_cmd(
    ctx: click.Context,
    input_c: str,
    energy_config: str | None,
    schematic_config: str | None,
    cap: str | None,
    output: str | None,
    link: bool,
    debug: bool,
    device_debug: bool,
    halt_mode: str,
    trace_file: str | None,
    trace_only: bool,
    opt_level: int,
    clang_opt_level: int,
    extra_includes: tuple[str, ...],
    estimator_mode: str,
    cpu_freq: str,
    save_temps: bool,
    accumulate_keys: str | None,
    force_checkpoint_on_incompatible_loops: bool,
    recompute_energy_after_new_checkpoint: bool,
) -> None:
    """Run the SCHEMATIC trace-based compilation pipeline.

    INPUT_C can be a benchmark name (e.g. "crc") or a path to a C file.
    """
    _compile_schematic_impl(
        ctx, input_c, energy_config, schematic_config, cap, output,
        link, debug, device_debug, halt_mode, trace_file, trace_only,
        opt_level, clang_opt_level, extra_includes, estimator_mode,
        cpu_freq, save_temps, algorithm_label="schematic",
        accumulate_keys=accumulate_keys,
        force_checkpoint_on_incompatible_loops=force_checkpoint_on_incompatible_loops,
        recompute_energy_after_new_checkpoint=recompute_energy_after_new_checkpoint,
    )


@compile.command("schematicO3")
@click.argument("input_c")
@click.option("-e", "--energy-config", type=click.Path(exists=True),
              help="Energy config (default: auto-detected by estimator mode).")
@click.option("-s", "--schematic-config", type=click.Path(exists=True),
              help="SCHEMATIC config (alternative to --cap).")
@click.option("--cap", help="Capacitor size (e.g. 1uF) — resolves to benchmarks/config_{cap}.json.")
@click.option("-o", "--output", type=click.Path())
@click.option("--link", is_flag=True, help="Link with boot.S and runtime.")
@click.option("--debug", is_flag=True, help="Enable DEBUG output.")
@click.option("--device-debug", is_flag=True, help="Enable device debug.")
@click.option(
    "--halt-mode",
    type=click.Choice(["nop", "bor", "lpm4", "swbor"]),
    default="nop",
    help="Halt mode for linked binary.",
)
@click.option(
    "--trace-file", type=click.Path(exists=True), help="Pre-collected trace JSON."
)
@click.option("--trace-only", is_flag=True, help="Only collect trace, skip insertion.")
@click.option("-O", "opt_level", type=int, default=3, help="LLC opt level.")
@click.option("-Oc", "clang_opt_level", type=int, default=3, help="Clang opt level.")
@click.option("-I", "extra_includes", multiple=True, help="Extra include dirs.")
@click.option(
    "--estimator-mode",
    type=click.Choice(["assembly", "ir"]),
    default="assembly",
    help="Energy estimator mode.",
)
@click.option(
    "--cpu-freq",
    type=click.Choice(["1", "8", "16"]),
    default="1",
    help="CPU frequency in MHz (default: 1).",
)
@click.option("--save-temps", is_flag=True, help="Save intermediate files to output directory.")
@click.option("--accumulate-keys", type=click.Path(), help="Accumulate required energy keys to this file.")
@click.option("--force-checkpoint-on-incompatible-loops", is_flag=True,
              help="Force checkpoint at loop header when inner loop allocations conflict.")
@click.option("--recompute-energy-after-new-checkpoint", is_flag=True,
              help="Recompute local E_left/E_to_leave after inserting a new checkpoint (disabled by default; deviates from the reference implementation).")
@click.pass_context
def compile_schematic_o3_cmd(
    ctx: click.Context,
    input_c: str,
    energy_config: str | None,
    schematic_config: str | None,
    cap: str | None,
    output: str | None,
    link: bool,
    debug: bool,
    device_debug: bool,
    halt_mode: str,
    trace_file: str | None,
    trace_only: bool,
    opt_level: int,
    clang_opt_level: int,
    extra_includes: tuple[str, ...],
    estimator_mode: str,
    cpu_freq: str,
    save_temps: bool,
    accumulate_keys: str | None,
    force_checkpoint_on_incompatible_loops: bool,
    recompute_energy_after_new_checkpoint: bool,
) -> None:
    """Run the SCHEMATIC-O3 trace-based compilation pipeline (clang -O3).

    INPUT_C can be a benchmark name (e.g. "crc") or a path to a C file.
    """
    _compile_schematic_impl(
        ctx, input_c, energy_config, schematic_config, cap, output,
        link, debug, device_debug, halt_mode, trace_file, trace_only,
        opt_level, clang_opt_level, extra_includes, estimator_mode,
        cpu_freq, save_temps, algorithm_label="schematicO3",
        accumulate_keys=accumulate_keys,
        force_checkpoint_on_incompatible_loops=force_checkpoint_on_incompatible_loops,
        recompute_energy_after_new_checkpoint=recompute_energy_after_new_checkpoint,
    )


@compile.command("uninstrumented")
@click.argument("input_c")
@click.option("-o", "--output", type=click.Path())
@click.option("--link/--no-link", default=True, help="Link with boot.S and runtime (default: on).")
@click.option("--device-debug/--no-device-debug", default=True, help="Enable device debug (default: on).")
@click.option("-O", "opt_level", type=int, default=3, help="LLC opt level.")
@click.option("-Oc", "clang_opt_level", type=int, default=3, help="Clang opt level.")
@click.option("-I", "extra_includes", multiple=True, help="Extra include dirs.")
@click.option(
    "--cpu-freq",
    type=click.Choice(["1", "8", "16"]),
    default="1",
    help="CPU frequency in MHz (default: 1).",
)
@click.pass_context
def compile_uninstrumented_cmd(
    ctx: click.Context,
    input_c: str,
    output: str | None,
    link: bool,
    device_debug: bool,
    opt_level: int,
    clang_opt_level: int,
    extra_includes: tuple[str, ...],
    cpu_freq: str,
) -> None:
    """Compile without checkpoint insertion (baseline).

    INPUT_C can be a benchmark name (e.g. "crc") or a path to a C file.
    """
    from .compile.uninstrumented import UninstrumentedCompileOptions, compile_uninstrumented

    input_path = _resolve_input(ctx.obj["env"], input_c)
    output_path = Path(output) if output else Path("build") / input_path.stem
    cpu_freq_hz = int(cpu_freq) * 1_000_000

    result = compile_uninstrumented(
        ctx.obj["tc"],
        ctx.obj["env"],
        UninstrumentedCompileOptions(
            input_c=input_path,
            output=output_path,
            device_debug=device_debug,
            cpu_freq=cpu_freq_hz,
            opt_level=opt_level,
            clang_opt_level=clang_opt_level,
            link=link,
            extra_includes=list(extra_includes),
        ),
    )

    logger.info("Object: %s", result.object_file)
    logger.info("Assembly: %s", result.assembly_file)
    if result.elf_file:
        logger.info("ELF: %s", result.elf_file)


# =========================================================================
# bench group
# =========================================================================

@main.group()
def bench() -> None:
    """Run benchmarks."""


@bench.command("milp")
@click.argument("benchmarks", nargs=-1)
@click.option("--cap", multiple=True, help="Capacitor sizes (e.g., 1uF 10uF).")
@click.option("--device-debug/--no-device-debug", default=True, help="Enable device debug (default: on).")
@click.option("-o", "--output", type=click.Path(), help="Output CSV path.")
@click.option(
    "--halt-mode",
    type=click.Choice(["nop", "bor", "lpm4", "swbor"]),
    default="swbor",
    help="Halt mode for linked binary.",
)
@click.option(
    "--estimator-mode",
    type=click.Choice(["assembly", "ir"]),
    default="assembly",
    help="Energy estimator mode.",
)
@click.option(
    "-e",
    "--energy-config",
    type=click.Path(exists=True),
    help="Override default energy config.",
)
@click.option(
    "--cpu-freq",
    type=click.Choice(["1", "8", "16"]),
    default="16",
    help="CPU frequency in MHz (default: 16).",
)
@click.option("--coarse-allocation", is_flag=True,
              help="Use one MILP placement variable per eligible value instead of per-region placement.")
@click.option("--accumulate-keys", type=click.Path(), help="Accumulate required energy keys to this file.")
@click.pass_context
def bench_milp_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    cap: tuple[str, ...],
    device_debug: bool,
    output: str | None,
    halt_mode: str,
    estimator_mode: str,
    energy_config: str | None,
    cpu_freq: str,
    coarse_allocation: bool,
    accumulate_keys: str | None,
) -> None:
    """Run MILP benchmarks across programs and capacitor sizes."""
    from .bench.milp import run_milp_benchmarks

    run_milp_benchmarks(
        ctx.obj["env"],
        ctx.obj["tc"],
        benchmarks=list(benchmarks) if benchmarks else None,
        caps=list(cap) if cap else None,
        device_debug=device_debug,
        halt_mode=halt_mode,
        output_csv=Path(output) if output else None,
        estimator_mode=estimator_mode,
        energy_config=Path(energy_config) if energy_config else None,
        cpu_freq=int(cpu_freq) * 1_000_000,
        coarse_allocation=coarse_allocation,
        pass_log_level=ctx.obj["pass_log_level"],
        accumulate_keys_file=Path(accumulate_keys) if accumulate_keys else None,
    )


@bench.command("rockclimb")
@click.argument("benchmarks", nargs=-1)
@click.option("--cap", multiple=True, help="Capacitor sizes (e.g., 1uF 10uF).")
@click.option("--device-debug/--no-device-debug", default=True, help="Enable device debug (default: on).")
@click.option("-o", "--output", type=click.Path(), help="Output CSV path.")
@click.option(
    "--halt-mode",
    type=click.Choice(["nop", "bor", "lpm4", "swbor"]),
    default="swbor",
    help="Halt mode for linked binary.",
)
@click.option(
    "-e",
    "--energy-config",
    type=click.Path(exists=True),
    help="Override default energy config.",
)
@click.option(
    "--cpu-freq",
    type=click.Choice(["1", "8", "16"]),
    default="16",
    help="CPU frequency in MHz (default: 16).",
)
@click.option("--accumulate-keys", type=click.Path(), help="Accumulate required energy keys to this file.")
@click.pass_context
def bench_rockclimb_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    cap: tuple[str, ...],
    device_debug: bool,
    output: str | None,
    halt_mode: str,
    energy_config: str | None,
    cpu_freq: str,
    accumulate_keys: str | None,
) -> None:
    """Run RockClimb benchmarks across programs and capacitor sizes."""
    from .bench.rockclimb import run_rockclimb_benchmarks

    run_rockclimb_benchmarks(
        ctx.obj["env"],
        ctx.obj["tc"],
        benchmarks=list(benchmarks) if benchmarks else None,
        caps=list(cap) if cap else None,
        device_debug=device_debug,
        halt_mode=halt_mode,
        output_csv=Path(output) if output else None,
        energy_config=Path(energy_config) if energy_config else None,
        cpu_freq=int(cpu_freq) * 1_000_000,
        pass_log_level=ctx.obj["pass_log_level"],
        accumulate_keys_file=Path(accumulate_keys) if accumulate_keys else None,
    )


@bench.command("schematic")
@click.argument("benchmarks", nargs=-1)
@click.option("--cap", multiple=True, help="Capacitor sizes (e.g., 1uF 10uF).")
@click.option("--device-debug/--no-device-debug", default=True, help="Enable device debug (default: on).")
@click.option("-o", "--output", type=click.Path(), help="Output CSV path.")
@click.option(
    "--halt-mode",
    type=click.Choice(["nop", "bor", "lpm4", "swbor"]),
    default="swbor",
    help="Halt mode for linked binary.",
)
@click.option(
    "-e",
    "--energy-config",
    type=click.Path(exists=True),
    help="Override default energy config.",
)
@click.option(
    "--trace-config",
    type=click.Path(exists=True),
    help="Override trace-collection config (default: config_10uF.json).",
)
@click.option(
    "--estimator-mode",
    type=click.Choice(["assembly", "ir"]),
    default="assembly",
    help="Energy estimator mode.",
)
@click.option(
    "--cpu-freq",
    type=click.Choice(["1", "8", "16"]),
    default="16",
    help="CPU frequency in MHz (default: 16).",
)
@click.option("--accumulate-keys", type=click.Path(), help="Accumulate required energy keys to this file.")
@click.option("--force-checkpoint-on-incompatible-loops", is_flag=True,
              help="Force checkpoint at loop header when inner loop allocations conflict.")
@click.option("--recompute-energy-after-new-checkpoint", is_flag=True,
              help="Recompute local E_left/E_to_leave after inserting a new checkpoint (disabled by default; deviates from the reference implementation).")
@click.pass_context
def bench_schematic_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    cap: tuple[str, ...],
    device_debug: bool,
    output: str | None,
    halt_mode: str,
    energy_config: str | None,
    trace_config: str | None,
    estimator_mode: str,
    cpu_freq: str,
    accumulate_keys: str | None,
    force_checkpoint_on_incompatible_loops: bool,
    recompute_energy_after_new_checkpoint: bool,
) -> None:
    """Run SCHEMATIC benchmarks across programs and capacitor sizes."""
    from .bench.schematic import run_schematic_benchmarks

    run_schematic_benchmarks(
        ctx.obj["env"],
        ctx.obj["tc"],
        benchmarks=list(benchmarks) if benchmarks else None,
        caps=list(cap) if cap else None,
        device_debug=device_debug,
        halt_mode=halt_mode,
        output_csv=Path(output) if output else None,
        energy_config=Path(energy_config) if energy_config else None,
        trace_config=Path(trace_config) if trace_config else None,
        estimator_mode=estimator_mode,
        cpu_freq=int(cpu_freq) * 1_000_000,
        clang_opt_level=0,
        pass_log_level=ctx.obj["pass_log_level"],
        algorithm_label="schematic",
        accumulate_keys_file=Path(accumulate_keys) if accumulate_keys else None,
        force_checkpoint_on_incompatible_loops=force_checkpoint_on_incompatible_loops,
        recompute_energy_after_new_checkpoint=recompute_energy_after_new_checkpoint,
    )


@bench.command("schematicO3")
@click.argument("benchmarks", nargs=-1)
@click.option("--cap", multiple=True, help="Capacitor sizes (e.g., 1uF 10uF).")
@click.option("--device-debug/--no-device-debug", default=True, help="Enable device debug (default: on).")
@click.option("-o", "--output", type=click.Path(), help="Output CSV path.")
@click.option(
    "--halt-mode",
    type=click.Choice(["nop", "bor", "lpm4", "swbor"]),
    default="swbor",
    help="Halt mode for linked binary.",
)
@click.option(
    "-e",
    "--energy-config",
    type=click.Path(exists=True),
    help="Override default energy config.",
)
@click.option(
    "--trace-config",
    type=click.Path(exists=True),
    help="Override trace-collection config (default: config_10uF.json).",
)
@click.option(
    "--estimator-mode",
    type=click.Choice(["assembly", "ir"]),
    default="assembly",
    help="Energy estimator mode.",
)
@click.option(
    "--cpu-freq",
    type=click.Choice(["1", "8", "16"]),
    default="16",
    help="CPU frequency in MHz (default: 16).",
)
@click.option("--accumulate-keys", type=click.Path(), help="Accumulate required energy keys to this file.")
@click.option("--force-checkpoint-on-incompatible-loops", is_flag=True,
              help="Force checkpoint at loop header when inner loop allocations conflict.")
@click.option("--recompute-energy-after-new-checkpoint", is_flag=True,
              help="Recompute local E_left/E_to_leave after inserting a new checkpoint (disabled by default; deviates from the reference implementation).")
@click.pass_context
def bench_schematic_o3_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    cap: tuple[str, ...],
    device_debug: bool,
    output: str | None,
    halt_mode: str,
    energy_config: str | None,
    trace_config: str | None,
    estimator_mode: str,
    cpu_freq: str,
    accumulate_keys: str | None,
    force_checkpoint_on_incompatible_loops: bool,
    recompute_energy_after_new_checkpoint: bool,
) -> None:
    """Run SCHEMATIC-O3 benchmarks across programs and capacitor sizes."""
    from .bench.schematic import run_schematic_benchmarks

    run_schematic_benchmarks(
        ctx.obj["env"],
        ctx.obj["tc"],
        benchmarks=list(benchmarks) if benchmarks else None,
        caps=list(cap) if cap else None,
        device_debug=device_debug,
        halt_mode=halt_mode,
        output_csv=Path(output) if output else None,
        energy_config=Path(energy_config) if energy_config else None,
        trace_config=Path(trace_config) if trace_config else None,
        estimator_mode=estimator_mode,
        cpu_freq=int(cpu_freq) * 1_000_000,
        clang_opt_level=3,
        pass_log_level=ctx.obj["pass_log_level"],
        algorithm_label="schematicO3",
        accumulate_keys_file=Path(accumulate_keys) if accumulate_keys else None,
        force_checkpoint_on_incompatible_loops=force_checkpoint_on_incompatible_loops,
        recompute_energy_after_new_checkpoint=recompute_energy_after_new_checkpoint,
    )


@bench.command("uninstrumented")
@click.argument("benchmarks", nargs=-1)
@click.option("-o", "--output", type=click.Path(), help="Output CSV path.")
@click.option(
    "--cpu-freq",
    type=click.Choice(["1", "8", "16"]),
    default="16",
    help="CPU frequency in MHz (default: 16).",
)
@click.pass_context
def bench_uninstrumented_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    output: str | None,
    cpu_freq: str,
) -> None:
    """Run uninstrumented baselines and measure execution time."""
    from .bench.uninstrumented import run_uninstrumented_benchmarks

    run_uninstrumented_benchmarks(
        ctx.obj["env"],
        ctx.obj["tc"],
        benchmarks=list(benchmarks) if benchmarks else None,
        output_csv=Path(output) if output else None,
        cpu_freq=int(cpu_freq) * 1_000_000,
    )


# =========================================================================
# verify group
# =========================================================================

@main.group()
def verify() -> None:
    """Verification commands."""


@verify.command("rockclimb")
@click.argument("benchmarks", nargs=-1)
@click.option("--cap", multiple=True, help="Capacitor sizes (e.g., 1uF 10uF).")
@click.option(
    "-e",
    "--energy-config",
    type=click.Path(exists=True),
    help="Override default energy config.",
)
# bor is required: verify tests correctness under resets (intermittent computing).
# Do NOT change to nop — the whole point is to exercise the checkpoint/restore
# boot path triggered by BOR.
@click.option(
    "--halt-mode",
    type=click.Choice(["nop", "bor", "lpm4", "swbor"]),
    default="bor",
    help="Halt mode for linked binary (default: bor).",
)
@click.option(
    "--cpu-freq",
    type=click.Choice(["1", "8", "16"]),
    default="1",
    help="CPU frequency in MHz (default: 1).",
)
@click.pass_context
def verify_rockclimb_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    cap: tuple[str, ...],
    energy_config: str | None,
    halt_mode: str,
    cpu_freq: str,
) -> None:
    """Verify semantic correctness of RockClimb checkpoint insertion."""
    from .verify.rockclimb import verify_rockclimb

    success = verify_rockclimb(
        ctx.obj["env"],
        ctx.obj["tc"],
        benchmarks=list(benchmarks) if benchmarks else None,
        caps=list(cap) if cap else None,
        halt_mode=halt_mode,
        energy_config=Path(energy_config) if energy_config else None,
        cpu_freq=int(cpu_freq) * 1_000_000,
        pass_log_level=ctx.obj["pass_log_level"],
    )
    if not success:
        raise SystemExit(1)


@verify.command("milp")
@click.argument("benchmarks", nargs=-1)
@click.option("--cap", multiple=True, help="Capacitor sizes (e.g., 1uF 10uF).")
@click.option(
    "-e",
    "--energy-config",
    type=click.Path(exists=True),
    help="Override default energy config.",
)
# bor is required: verify tests correctness under resets (intermittent computing).
@click.option(
    "--halt-mode",
    type=click.Choice(["nop", "bor", "lpm4", "swbor"]),
    default="bor",
    help="Halt mode for linked binary (default: bor).",
)
@click.option(
    "--estimator-mode",
    type=click.Choice(["assembly", "ir"]),
    default="assembly",
    help="Energy estimator mode.",
)
@click.option(
    "--cpu-freq",
    type=click.Choice(["1", "8", "16"]),
    default="1",
    help="CPU frequency in MHz (default: 1).",
)
@click.option("--coarse-allocation", is_flag=True,
              help="Use one MILP placement variable per eligible value instead of per-region placement.")
@click.pass_context
def verify_milp_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    cap: tuple[str, ...],
    energy_config: str | None,
    halt_mode: str,
    estimator_mode: str,
    cpu_freq: str,
    coarse_allocation: bool,
) -> None:
    """Verify semantic correctness of MILP checkpoint insertion."""
    from .verify.milp import verify_milp

    success = verify_milp(
        ctx.obj["env"],
        ctx.obj["tc"],
        benchmarks=list(benchmarks) if benchmarks else None,
        caps=list(cap) if cap else None,
        halt_mode=halt_mode,
        energy_config=Path(energy_config) if energy_config else None,
        estimator_mode=estimator_mode,
        cpu_freq=int(cpu_freq) * 1_000_000,
        coarse_allocation=coarse_allocation,
        pass_log_level=ctx.obj["pass_log_level"],
    )
    if not success:
        raise SystemExit(1)


@verify.command("schematic")
@click.argument("benchmarks", nargs=-1)
@click.option("--cap", multiple=True, help="Capacitor sizes (e.g., 1uF 10uF).")
@click.option(
    "-e",
    "--energy-config",
    type=click.Path(exists=True),
    help="Override default energy config.",
)
# bor is required: verify tests correctness under resets (intermittent computing).
@click.option(
    "--halt-mode",
    type=click.Choice(["nop", "bor", "lpm4", "swbor"]),
    default="bor",
    help="Halt mode for linked binary (default: bor).",
)
@click.option(
    "--estimator-mode",
    type=click.Choice(["assembly", "ir"]),
    default="assembly",
    help="Energy estimator mode.",
)
@click.option(
    "--cpu-freq",
    type=click.Choice(["1", "8", "16"]),
    default="1",
    help="CPU frequency in MHz (default: 1).",
)
@click.option("--force-checkpoint-on-incompatible-loops", is_flag=True,
              help="Force checkpoint at loop header when inner loop allocations conflict.")
@click.option("--recompute-energy-after-new-checkpoint", is_flag=True,
              help="Recompute local E_left/E_to_leave after inserting a new checkpoint (disabled by default; deviates from the reference implementation).")
@click.pass_context
def verify_schematic_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    cap: tuple[str, ...],
    energy_config: str | None,
    halt_mode: str,
    estimator_mode: str,
    cpu_freq: str,
    force_checkpoint_on_incompatible_loops: bool,
    recompute_energy_after_new_checkpoint: bool,
) -> None:
    """Verify semantic correctness of SCHEMATIC checkpoint insertion."""
    from .verify.schematic import verify_schematic

    success = verify_schematic(
        ctx.obj["env"],
        ctx.obj["tc"],
        benchmarks=list(benchmarks) if benchmarks else None,
        caps=list(cap) if cap else None,
        halt_mode=halt_mode,
        energy_config=Path(energy_config) if energy_config else None,
        estimator_mode=estimator_mode,
        cpu_freq=int(cpu_freq) * 1_000_000,
        clang_opt_level=0,
        pass_log_level=ctx.obj["pass_log_level"],
        algorithm_label="schematic",
        force_checkpoint_on_incompatible_loops=force_checkpoint_on_incompatible_loops,
        recompute_energy_after_new_checkpoint=recompute_energy_after_new_checkpoint,
    )
    if not success:
        raise SystemExit(1)


@verify.command("schematicO3")
@click.argument("benchmarks", nargs=-1)
@click.option("--cap", multiple=True, help="Capacitor sizes (e.g., 1uF 10uF).")
@click.option(
    "-e",
    "--energy-config",
    type=click.Path(exists=True),
    help="Override default energy config.",
)
# bor is required: verify tests correctness under resets (intermittent computing).
@click.option(
    "--halt-mode",
    type=click.Choice(["nop", "bor", "lpm4", "swbor"]),
    default="bor",
    help="Halt mode for linked binary (default: bor).",
)
@click.option(
    "--estimator-mode",
    type=click.Choice(["assembly", "ir"]),
    default="assembly",
    help="Energy estimator mode.",
)
@click.option(
    "--cpu-freq",
    type=click.Choice(["1", "8", "16"]),
    default="1",
    help="CPU frequency in MHz (default: 1).",
)
@click.option("--force-checkpoint-on-incompatible-loops", is_flag=True,
              help="Force checkpoint at loop header when inner loop allocations conflict.")
@click.option("--recompute-energy-after-new-checkpoint", is_flag=True,
              help="Recompute local E_left/E_to_leave after inserting a new checkpoint (disabled by default; deviates from the reference implementation).")
@click.pass_context
def verify_schematic_o3_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    cap: tuple[str, ...],
    energy_config: str | None,
    halt_mode: str,
    estimator_mode: str,
    cpu_freq: str,
    force_checkpoint_on_incompatible_loops: bool,
    recompute_energy_after_new_checkpoint: bool,
) -> None:
    """Verify semantic correctness of SCHEMATIC-O3 checkpoint insertion."""
    from .verify.schematic import verify_schematic

    success = verify_schematic(
        ctx.obj["env"],
        ctx.obj["tc"],
        benchmarks=list(benchmarks) if benchmarks else None,
        caps=list(cap) if cap else None,
        halt_mode=halt_mode,
        energy_config=Path(energy_config) if energy_config else None,
        estimator_mode=estimator_mode,
        cpu_freq=int(cpu_freq) * 1_000_000,
        clang_opt_level=3,
        pass_log_level=ctx.obj["pass_log_level"],
        algorithm_label="schematicO3",
        force_checkpoint_on_incompatible_loops=force_checkpoint_on_incompatible_loops,
        recompute_energy_after_new_checkpoint=recompute_energy_after_new_checkpoint,
    )
    if not success:
        raise SystemExit(1)


# =========================================================================
# analyze group
# =========================================================================

@main.group()
def analyze() -> None:
    """Analysis commands."""


@analyze.command("strip-mining")
@click.argument("log_file", type=click.Path(exists=True))
@click.option(
    "-o",
    "--output",
    type=click.Path(),
    default="loop_strip_mining_vs_capacitor.csv",
    help="Output CSV path.",
)
def analyze_strip_mining_cmd(log_file: str, output: str) -> None:
    """Parse verbose MILP log for strip-mining K values."""
    from .analysis.strip_mining import parse_strip_mining_log, write_strip_mining_csv

    runs = parse_strip_mining_log(Path(log_file))
    write_strip_mining_csv(runs, Path(output))


@analyze.command("plot")
@click.argument("csv_dir", type=click.Path(exists=True))
@click.option(
    "--metric",
    type=click.Choice(list(METRICS.keys())),
    default="prologue",
    help="Metric to plot.",
)
@click.option(
    "--algorithms",
    multiple=True,
    type=click.Choice(list(ALGORITHMS.keys())),
    help="Algorithms to include (default: all).",
)
@click.option("--benchmarks", multiple=True, help="Benchmark programs to include.")
@click.option("--capacitors", multiple=True, help="Capacitor sizes to include.")
@click.option(
    "--normalize",
    type=click.Choice(list(ALGORITHMS.keys())),
    help="Normalize values relative to this algorithm.",
)
@click.option("-o", "--output", type=click.Path(), help="Output file (e.g., plot.png).")
def analyze_plot_cmd(
    csv_dir: str,
    metric: str,
    algorithms: tuple[str, ...],
    benchmarks: tuple[str, ...],
    capacitors: tuple[str, ...],
    normalize: str | None,
    output: str | None,
) -> None:
    """Plot benchmark comparison chart from CSV results."""
    from .analysis.plot import plot_benchmarks

    plot_benchmarks(
        csv_dir=Path(csv_dir),
        metric=metric,
        algorithms=list(algorithms) if algorithms else None,
        benchmarks=list(benchmarks) if benchmarks else None,
        capacitors=list(capacitors) if capacitors else None,
        normalize=normalize,
        output_file=Path(output) if output else None,
    )


# =========================================================================
# device group
# =========================================================================

@main.group()
def device() -> None:
    """Device interaction."""




@device.command("read-serial")
@click.option("--timeout", type=float, default=30, help="Read timeout in seconds.")
@click.option(
    "--end-marker",
    default="END_OUTPUT",
    help="Marker string that terminates reading.",
)
def device_read_serial_cmd(timeout: float, end_marker: str) -> None:
    """Read serial output from a connected MSP430 device."""
    from .device.serial import read_serial_output

    output = read_serial_output(timeout=timeout, end_marker=end_marker)
    click.echo(output)

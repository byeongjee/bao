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
from .log import setup_logging

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


# =========================================================================
# compile group
# =========================================================================

@main.group()
def compile() -> None:
    """Compilation pipelines."""


@compile.command("milp")
@click.argument("input_c", type=click.Path(exists=True))
@click.option("-e", "--energy-config", required=True, type=click.Path(exists=True))
@click.option("-m", "--milp-config", required=True, type=click.Path(exists=True))
@click.option("-o", "--output", type=click.Path())
@click.option("--link", is_flag=True, help="Link with boot.S and runtime.")
@click.option(
    "--estimator-mode",
    type=click.Choice(["assembly", "ir"]),
    default="assembly",
    help="Energy estimator mode.",
)
@click.option("--debug", is_flag=True, help="Enable DEBUG output.")
@click.option("--debug-counters", is_flag=True, help="Enable debug counters.")
@click.option(
    "--halt-mode",
    type=click.Choice(["nop", "bor", "lpm4"]),
    default="nop",
    help="Halt mode for linked binary.",
)
@click.option("-O", "opt_level", type=int, default=2, help="LLC opt level.")
@click.option("-Oc", "clang_opt_level", type=int, default=2, help="Clang opt level.")
@click.option("-I", "extra_includes", multiple=True, help="Extra include dirs.")
@click.option(
    "--cpu-freq",
    type=click.Choice(["1", "8", "16"]),
    default="1",
    help="CPU frequency in MHz (default: 1).",
)
@click.pass_context
def compile_milp_cmd(
    ctx: click.Context,
    input_c: str,
    energy_config: str,
    milp_config: str,
    output: str | None,
    link: bool,
    estimator_mode: str,
    debug: bool,
    debug_counters: bool,
    halt_mode: str,
    opt_level: int,
    clang_opt_level: int,
    extra_includes: tuple[str, ...],
    cpu_freq: str,
) -> None:
    """Run the MILP checkpoint insertion compilation pipeline."""
    from .compile.milp import MilpCompileOptions, compile_milp

    input_path = Path(input_c)
    output_path = Path(output) if output else Path("build") / input_path.stem
    cpu_freq_hz = int(cpu_freq) * 1_000_000

    result = compile_milp(
        ctx.obj["tc"],
        ctx.obj["env"],
        MilpCompileOptions(
            input_c=input_path,
            energy_config=Path(energy_config),
            milp_config=Path(milp_config),
            output=output_path,
            opt_level=opt_level,
            clang_opt_level=clang_opt_level,
            extra_includes=list(extra_includes),
            pass_log_level=ctx.obj["log_level"],
            debug=debug,
            link=link,
            halt_mode=halt_mode,
            debug_counters=debug_counters,
            estimator_mode=estimator_mode,
            cpu_freq=cpu_freq_hz,
        ),
    )

    logger.info("Object: %s", result.object_file)
    logger.info("Assembly: %s", result.assembly_file)
    if result.elf_file:
        logger.info("ELF: %s", result.elf_file)
    logger.debug("Pass output:\n%s", result.pass_output)


@compile.command("rockclimb")
@click.argument("input_c", type=click.Path(exists=True))
@click.option("-e", "--energy-config", required=True, type=click.Path(exists=True))
@click.option("-c", "--rockclimb-config", required=True, type=click.Path(exists=True))
@click.option("-o", "--output", type=click.Path())
@click.option("--link", is_flag=True, help="Link with boot.S and runtime.")
@click.option("--debug-counters", is_flag=True, help="Enable debug counters.")
@click.option(
    "--halt-mode",
    type=click.Choice(["nop", "bor", "lpm4"]),
    default="nop",
    help="Halt mode for linked binary.",
)
@click.option("-Oc", "clang_opt_level", type=int, default=2, help="Clang opt level.")
@click.option(
    "--no-precomputed-energy",
    is_flag=True,
    help="Use MIR-level estimation instead of assembly-based.",
)
@click.option(
    "--cpu-freq",
    type=click.Choice(["1", "8", "16"]),
    default="1",
    help="CPU frequency in MHz (default: 1).",
)
@click.pass_context
def compile_rockclimb_cmd(
    ctx: click.Context,
    input_c: str,
    energy_config: str,
    rockclimb_config: str,
    output: str | None,
    link: bool,
    debug_counters: bool,
    halt_mode: str,
    clang_opt_level: int,
    no_precomputed_energy: bool,
    cpu_freq: str,
) -> None:
    """Run the RockClimb machine-level compilation pipeline."""
    from .compile.rockclimb import RockClimbCompileOptions, compile_rockclimb

    input_path = Path(input_c)
    output_path = Path(output) if output else Path("build") / input_path.stem
    cpu_freq_hz = int(cpu_freq) * 1_000_000

    result = compile_rockclimb(
        ctx.obj["tc"],
        ctx.obj["env"],
        RockClimbCompileOptions(
            input_c=input_path,
            energy_config=Path(energy_config),
            rockclimb_config=Path(rockclimb_config),
            output=output_path,
            pass_log_level=ctx.obj["log_level"],
            clang_opt_level=clang_opt_level,
            precomputed_energy=not no_precomputed_energy,
            link=link,
            debug_counters=debug_counters,
            halt_mode=halt_mode,
            cpu_freq=cpu_freq_hz,
        ),
    )

    logger.info("Assembly: %s", result.assembly_file)
    if result.elf_file:
        logger.info("ELF: %s", result.elf_file)
    logger.debug("Pass output:\n%s", result.pass_output)


@compile.command("schematic")
@click.argument("input_c", type=click.Path(exists=True))
@click.option("-e", "--energy-config", required=True, type=click.Path(exists=True))
@click.option("-s", "--schematic-config", type=click.Path(exists=True))
@click.option("-o", "--output", type=click.Path())
@click.option("--link", is_flag=True, help="Link with boot.S and runtime.")
@click.option("--debug", is_flag=True, help="Enable DEBUG output.")
@click.option("--debug-counters", is_flag=True, help="Enable debug counters.")
@click.option(
    "--halt-mode",
    type=click.Choice(["nop", "bor", "lpm4"]),
    default="nop",
    help="Halt mode for linked binary.",
)
@click.option(
    "--trace-file", type=click.Path(exists=True), help="Pre-collected trace JSON."
)
@click.option("--trace-only", is_flag=True, help="Only collect trace, skip insertion.")
@click.option("-O", "opt_level", type=int, default=2, help="LLC opt level.")
@click.option("-Oc", "clang_opt_level", type=int, default=2, help="Clang opt level.")
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
@click.pass_context
def compile_schematic_cmd(
    ctx: click.Context,
    input_c: str,
    energy_config: str,
    schematic_config: str | None,
    output: str | None,
    link: bool,
    debug: bool,
    debug_counters: bool,
    halt_mode: str,
    trace_file: str | None,
    trace_only: bool,
    opt_level: int,
    clang_opt_level: int,
    extra_includes: tuple[str, ...],
    estimator_mode: str,
    cpu_freq: str,
) -> None:
    """Run the SCHEMATIC trace-based compilation pipeline."""
    from .compile.schematic import SchematicCompileOptions, compile_schematic

    input_path = Path(input_c)
    output_path = Path(output) if output else Path("build") / input_path.stem
    cpu_freq_hz = int(cpu_freq) * 1_000_000

    if not trace_only and schematic_config is None:
        raise click.UsageError(
            "--schematic-config is required unless --trace-only is set."
        )

    result = compile_schematic(
        ctx.obj["tc"],
        ctx.obj["env"],
        SchematicCompileOptions(
            input_c=input_path,
            energy_config=Path(energy_config),
            schematic_config=Path(schematic_config) if schematic_config else None,
            output=output_path,
            estimator_mode=estimator_mode,
            pass_log_level=ctx.obj["log_level"],
            debug=debug,
            trace_only=trace_only,
            link=link,
            debug_counters=debug_counters,
            halt_mode=halt_mode,
            cpu_freq=cpu_freq_hz,
            opt_level=opt_level,
            clang_opt_level=clang_opt_level,
            extra_includes=list(extra_includes),
            trace_file=Path(trace_file) if trace_file else None,
        ),
    )

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


@compile.command("uninstrumented")
@click.argument("input_c", type=click.Path(exists=True))
@click.option("-o", "--output", type=click.Path())
@click.option("--link/--no-link", default=True, help="Link with boot.S and runtime (default: on).")
@click.option("--debug-counters/--no-debug-counters", default=True, help="Enable debug counters (default: on).")
@click.option(
    "--halt-mode",
    type=click.Choice(["nop", "bor", "lpm4"]),
    default="nop",
    help="Halt mode for linked binary.",
)
@click.option("-O", "opt_level", type=int, default=2, help="LLC opt level.")
@click.option("-Oc", "clang_opt_level", type=int, default=2, help="Clang opt level.")
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
    debug_counters: bool,
    halt_mode: str,
    opt_level: int,
    clang_opt_level: int,
    extra_includes: tuple[str, ...],
    cpu_freq: str,
) -> None:
    """Compile without checkpoint insertion (baseline)."""
    from .compile.uninstrumented import UninstrumentedCompileOptions, compile_uninstrumented

    input_path = Path(input_c)
    output_path = Path(output) if output else Path("build") / input_path.stem
    cpu_freq_hz = int(cpu_freq) * 1_000_000

    result = compile_uninstrumented(
        ctx.obj["tc"],
        ctx.obj["env"],
        UninstrumentedCompileOptions(
            input_c=input_path,
            output=output_path,
            halt_mode=halt_mode,
            debug_counters=debug_counters,
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
@click.option("--debug-counters/--no-debug-counters", default=True, help="Enable debug counters (default: on).")
@click.option("-o", "--output", type=click.Path(), help="Output CSV path.")
@click.option(
    "--halt-mode",
    type=click.Choice(["nop", "bor", "lpm4"]),
    default="nop",
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
    default="1",
    help="CPU frequency in MHz (default: 1).",
)
@click.pass_context
def bench_milp_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    cap: tuple[str, ...],
    debug_counters: bool,
    output: str | None,
    halt_mode: str,
    estimator_mode: str,
    energy_config: str | None,
    cpu_freq: str,
) -> None:
    """Run MILP benchmarks across programs and capacitor sizes."""
    from .bench.milp import run_milp_benchmarks

    run_milp_benchmarks(
        ctx.obj["env"],
        ctx.obj["tc"],
        benchmarks=list(benchmarks) if benchmarks else None,
        caps=list(cap) if cap else None,
        debug_counters=debug_counters,
        halt_mode=halt_mode,
        output_csv=Path(output) if output else None,
        estimator_mode=estimator_mode,
        energy_config=Path(energy_config) if energy_config else None,
        cpu_freq=int(cpu_freq) * 1_000_000,
    )


@bench.command("rockclimb")
@click.argument("benchmarks", nargs=-1)
@click.option("--cap", multiple=True, help="Capacitor sizes (e.g., 1uF 10uF).")
@click.option("--debug-counters/--no-debug-counters", default=True, help="Enable debug counters (default: on).")
@click.option("-o", "--output", type=click.Path(), help="Output CSV path.")
@click.option(
    "--halt-mode",
    type=click.Choice(["nop", "bor", "lpm4"]),
    default="nop",
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
    default="1",
    help="CPU frequency in MHz (default: 1).",
)
@click.pass_context
def bench_rockclimb_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    cap: tuple[str, ...],
    debug_counters: bool,
    output: str | None,
    halt_mode: str,
    energy_config: str | None,
    cpu_freq: str,
) -> None:
    """Run RockClimb benchmarks across programs and capacitor sizes."""
    from .bench.rockclimb import run_rockclimb_benchmarks

    run_rockclimb_benchmarks(
        ctx.obj["env"],
        ctx.obj["tc"],
        benchmarks=list(benchmarks) if benchmarks else None,
        caps=list(cap) if cap else None,
        debug_counters=debug_counters,
        halt_mode=halt_mode,
        output_csv=Path(output) if output else None,
        energy_config=Path(energy_config) if energy_config else None,
        cpu_freq=int(cpu_freq) * 1_000_000,
    )


@bench.command("schematic")
@click.argument("benchmarks", nargs=-1)
@click.option("--cap", multiple=True, help="Capacitor sizes (e.g., 1uF 10uF).")
@click.option("--debug-counters/--no-debug-counters", default=True, help="Enable debug counters (default: on).")
@click.option("-o", "--output", type=click.Path(), help="Output CSV path.")
@click.option(
    "--halt-mode",
    type=click.Choice(["nop", "bor", "lpm4"]),
    default="nop",
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
    default="1",
    help="CPU frequency in MHz (default: 1).",
)
@click.pass_context
def bench_schematic_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    cap: tuple[str, ...],
    debug_counters: bool,
    output: str | None,
    halt_mode: str,
    energy_config: str | None,
    trace_config: str | None,
    estimator_mode: str,
    cpu_freq: str,
) -> None:
    """Run SCHEMATIC benchmarks across programs and capacitor sizes."""
    from .bench.schematic import run_schematic_benchmarks

    run_schematic_benchmarks(
        ctx.obj["env"],
        ctx.obj["tc"],
        benchmarks=list(benchmarks) if benchmarks else None,
        caps=list(cap) if cap else None,
        debug_counters=debug_counters,
        halt_mode=halt_mode,
        output_csv=Path(output) if output else None,
        energy_config=Path(energy_config) if energy_config else None,
        trace_config=Path(trace_config) if trace_config else None,
        estimator_mode=estimator_mode,
        cpu_freq=int(cpu_freq) * 1_000_000,
    )


@bench.command("uninstrumented")
@click.argument("benchmarks", nargs=-1)
@click.option("-o", "--output", type=click.Path(), help="Output CSV path.")
@click.option(
    "--halt-mode",
    type=click.Choice(["nop", "bor", "lpm4"]),
    default="nop",
    help="Halt mode for linked binary.",
)
@click.option(
    "--cpu-freq",
    type=click.Choice(["1", "8", "16"]),
    default="1",
    help="CPU frequency in MHz (default: 1).",
)
@click.pass_context
def bench_uninstrumented_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    output: str | None,
    halt_mode: str,
    cpu_freq: str,
) -> None:
    """Run uninstrumented baselines and measure execution time."""
    from .bench.uninstrumented import run_uninstrumented_benchmarks

    run_uninstrumented_benchmarks(
        ctx.obj["env"],
        ctx.obj["tc"],
        benchmarks=list(benchmarks) if benchmarks else None,
        halt_mode=halt_mode,
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
    type=click.Choice(["nop", "bor", "lpm4"]),
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
    type=click.Choice(["nop", "bor", "lpm4"]),
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
@click.pass_context
def verify_milp_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    cap: tuple[str, ...],
    energy_config: str | None,
    halt_mode: str,
    estimator_mode: str,
    cpu_freq: str,
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
    type=click.Choice(["nop", "bor", "lpm4"]),
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
@click.pass_context
def verify_schematic_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    cap: tuple[str, ...],
    energy_config: str | None,
    halt_mode: str,
    estimator_mode: str,
    cpu_freq: str,
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

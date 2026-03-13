"""Click-based CLI for the ckpt package.

Provides subcommands for compilation, benchmarking, verification,
analysis, and device interaction.
"""

from __future__ import annotations

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
@click.pass_context
def main(ctx: click.Context) -> None:
    """ckpt -- Checkpoint insertion toolchain."""
    from .env import ProjectEnv
    from .toolchain import Toolchain

    env = ProjectEnv.from_environ()
    tc = Toolchain.resolve(env)
    ctx.ensure_object(dict)
    ctx.obj["env"] = env
    ctx.obj["tc"] = tc


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
@click.option("--verbose", is_flag=True, help="Show detailed pass output.")
@click.option("--debug", is_flag=True, help="Enable DEBUG output.")
@click.option("--debug-counters", is_flag=True, help="Enable debug counters.")
@click.option("--add-debug-markers", is_flag=True, help="Add debug markers.")
@click.option(
    "--halt-mode",
    type=click.Choice(["nop", "bor", "lpm4"]),
    default="nop",
    help="Halt mode for linked binary.",
)
@click.option("-O", "opt_level", type=int, default=2, help="LLC opt level.")
@click.option("-Oc", "clang_opt_level", type=int, default=2, help="Clang opt level.")
@click.option("-I", "extra_includes", multiple=True, help="Extra include dirs.")
@click.pass_context
def compile_milp_cmd(
    ctx: click.Context,
    input_c: str,
    energy_config: str,
    milp_config: str,
    output: str | None,
    link: bool,
    estimator_mode: str,
    verbose: bool,
    debug: bool,
    debug_counters: bool,
    add_debug_markers: bool,
    halt_mode: str,
    opt_level: int,
    clang_opt_level: int,
    extra_includes: tuple[str, ...],
) -> None:
    """Run the MILP checkpoint insertion compilation pipeline."""
    from .compile.milp import MilpCompileOptions, compile_milp

    input_path = Path(input_c)
    output_path = Path(output) if output else Path("build") / input_path.stem

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
            verbose=verbose,
            debug=debug,
            add_debug_markers=add_debug_markers,
            link=link,
            halt_mode=halt_mode,
            debug_counters=debug_counters,
            estimator_mode=estimator_mode,
        ),
    )

    click.echo(f"Object: {result.object_file}")
    click.echo(f"Assembly: {result.assembly_file}")
    if result.elf_file:
        click.echo(f"ELF: {result.elf_file}")
    if verbose:
        click.echo(result.pass_output)


@compile.command("rockclimb")
@click.argument("input_c", type=click.Path(exists=True))
@click.option("-e", "--energy-config", required=True, type=click.Path(exists=True))
@click.option("-c", "--rockclimb-config", required=True, type=click.Path(exists=True))
@click.option("-o", "--output", type=click.Path())
@click.option("--link", is_flag=True, help="Link with boot.S and runtime.")
@click.option("--verbose", is_flag=True, help="Show detailed pass output.")
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
@click.pass_context
def compile_rockclimb_cmd(
    ctx: click.Context,
    input_c: str,
    energy_config: str,
    rockclimb_config: str,
    output: str | None,
    link: bool,
    verbose: bool,
    debug_counters: bool,
    halt_mode: str,
    clang_opt_level: int,
    no_precomputed_energy: bool,
) -> None:
    """Run the RockClimb machine-level compilation pipeline."""
    from .compile.rockclimb import RockClimbCompileOptions, compile_rockclimb

    input_path = Path(input_c)
    output_path = Path(output) if output else Path("build") / input_path.stem

    result = compile_rockclimb(
        ctx.obj["tc"],
        ctx.obj["env"],
        RockClimbCompileOptions(
            input_c=input_path,
            energy_config=Path(energy_config),
            rockclimb_config=Path(rockclimb_config),
            output=output_path,
            clang_opt_level=clang_opt_level,
            precomputed_energy=not no_precomputed_energy,
            verbose=verbose,
            link=link,
            debug_counters=debug_counters,
            halt_mode=halt_mode,
        ),
    )

    click.echo(f"Assembly: {result.assembly_file}")
    if result.elf_file:
        click.echo(f"ELF: {result.elf_file}")
    if verbose:
        click.echo(result.pass_output)


@compile.command("schematic")
@click.argument("input_c", type=click.Path(exists=True))
@click.option("-e", "--energy-config", required=True, type=click.Path(exists=True))
@click.option("-s", "--schematic-config", type=click.Path(exists=True))
@click.option("-o", "--output", type=click.Path())
@click.option("--link", is_flag=True, help="Link with boot.S and runtime.")
@click.option("--verbose", is_flag=True, help="Show detailed pass output.")
@click.option("--debug", is_flag=True, help="Enable DEBUG output.")
@click.option("--debug-counters", is_flag=True, help="Enable debug counters.")
@click.option("--add-debug-markers", is_flag=True, help="Add debug markers.")
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
@click.pass_context
def compile_schematic_cmd(
    ctx: click.Context,
    input_c: str,
    energy_config: str,
    schematic_config: str | None,
    output: str | None,
    link: bool,
    verbose: bool,
    debug: bool,
    debug_counters: bool,
    add_debug_markers: bool,
    halt_mode: str,
    trace_file: str | None,
    trace_only: bool,
    opt_level: int,
    clang_opt_level: int,
    extra_includes: tuple[str, ...],
    estimator_mode: str,
) -> None:
    """Run the SCHEMATIC trace-based compilation pipeline."""
    from .compile.schematic import SchematicCompileOptions, compile_schematic

    input_path = Path(input_c)
    output_path = Path(output) if output else Path("build") / input_path.stem

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
            verbose=verbose,
            debug=debug,
            add_debug_markers=add_debug_markers,
            trace_only=trace_only,
            link=link,
            debug_counters=debug_counters,
            halt_mode=halt_mode,
            opt_level=opt_level,
            clang_opt_level=clang_opt_level,
            extra_includes=list(extra_includes),
            trace_file=Path(trace_file) if trace_file else None,
        ),
    )

    if result.object_file:
        click.echo(f"Object: {result.object_file}")
    if result.assembly_file:
        click.echo(f"Assembly: {result.assembly_file}")
    if result.elf_file:
        click.echo(f"ELF: {result.elf_file}")
    if result.trace_file:
        click.echo(f"Trace: {result.trace_file}")
    if verbose and result.pass_output:
        click.echo(result.pass_output)


@compile.command("run")
@click.argument("input_c", type=click.Path(exists=True))
@click.option(
    "--mode",
    type=click.Choice(["none", "milp", "rockclimb", "schematic"]),
    default="none",
    help="Checkpoint insertion mode.",
)
@click.option("-e", "--energy-config", type=click.Path(exists=True))
@click.option("-m", "--milp-config", type=click.Path(exists=True))
@click.option("-c", "--rockclimb-config", type=click.Path(exists=True))
@click.option("-s", "--schematic-config", type=click.Path(exists=True))
@click.option("-t", "--trace-file", type=click.Path(exists=True))
@click.option("-o", "--output", type=click.Path())
@click.option("-O", "opt_level", type=int, default=2, help="LLC opt level.")
@click.option("-Oc", "clang_opt_level", type=int, default=2, help="Clang opt level.")
@click.option("-I", "extra_includes", multiple=True, help="Extra include dirs.")
@click.option("--verbose", is_flag=True, help="Show detailed pass output.")
@click.option("--debug", is_flag=True, help="Enable DEBUG output.")
@click.option("--debug-counters", is_flag=True, help="Enable debug counters.")
@click.option("--compile-only", is_flag=True, help="Compile but don't flash.")
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
    help="Energy estimator mode (MILP and SCHEMATIC).",
)
@click.pass_context
def compile_run_cmd(
    ctx: click.Context,
    input_c: str,
    mode: str,
    energy_config: str | None,
    milp_config: str | None,
    rockclimb_config: str | None,
    schematic_config: str | None,
    trace_file: str | None,
    output: str | None,
    opt_level: int,
    clang_opt_level: int,
    extra_includes: tuple[str, ...],
    verbose: bool,
    debug: bool,
    debug_counters: bool,
    compile_only: bool,
    halt_mode: str,
    estimator_mode: str,
) -> None:
    """Compile with checkpoint insertion and optionally flash.

    Unified entry point that dispatches to the appropriate compilation
    pipeline based on --mode.
    """
    input_path = Path(input_c)
    output_path = Path(output) if output else Path("build") / input_path.stem
    tc = ctx.obj["tc"]
    env = ctx.obj["env"]

    if mode == "milp":
        if not energy_config:
            raise click.UsageError("-e/--energy-config is required for MILP mode.")
        if not milp_config:
            raise click.UsageError("-m/--milp-config is required for MILP mode.")

        from .compile.milp import MilpCompileOptions, compile_milp

        result = compile_milp(
            tc, env,
            MilpCompileOptions(
                input_c=input_path,
                energy_config=Path(energy_config),
                milp_config=Path(milp_config),
                output=output_path,
                estimator_mode=estimator_mode,
                verbose=verbose,
                debug=debug,
                add_debug_markers=False,
                link=True,
                halt_mode=halt_mode,
                debug_counters=debug_counters,
                opt_level=opt_level,
                clang_opt_level=clang_opt_level,
                extra_includes=list(extra_includes),
            ),
        )
        elf = result.elf_file
        click.echo(f"MILP compiled: {result.object_file}")
        if verbose:
            click.echo(result.pass_output)

    elif mode == "rockclimb":
        if not energy_config:
            raise click.UsageError("-e/--energy-config is required for RockClimb mode.")
        if not rockclimb_config:
            raise click.UsageError("-c/--rockclimb-config is required for RockClimb mode.")

        from .compile.rockclimb import RockClimbCompileOptions, compile_rockclimb

        result = compile_rockclimb(
            tc, env,
            RockClimbCompileOptions(
                input_c=input_path,
                energy_config=Path(energy_config),
                rockclimb_config=Path(rockclimb_config),
                output=output_path,
                precomputed_energy=True,
                verbose=verbose,
                link=True,
                debug_counters=debug_counters,
                halt_mode=halt_mode,
                clang_opt_level=clang_opt_level,
            ),
        )
        elf = result.elf_file
        click.echo(f"RockClimb compiled: {result.assembly_file}")
        if verbose:
            click.echo(result.pass_output)

    elif mode == "schematic":
        if not energy_config:
            raise click.UsageError("-e/--energy-config is required for SCHEMATIC mode.")
        if not schematic_config:
            raise click.UsageError("-s/--schematic-config is required for SCHEMATIC mode.")

        from .compile.schematic import SchematicCompileOptions, compile_schematic

        result = compile_schematic(
            tc, env,
            SchematicCompileOptions(
                input_c=input_path,
                energy_config=Path(energy_config),
                schematic_config=Path(schematic_config),
                output=output_path,
                estimator_mode=estimator_mode,
                verbose=verbose,
                debug=debug,
                add_debug_markers=False,
                trace_only=False,
                link=True,
                debug_counters=debug_counters,
                halt_mode=halt_mode,
                opt_level=opt_level,
                clang_opt_level=clang_opt_level,
                extra_includes=list(extra_includes),
                trace_file=Path(trace_file) if trace_file else None,
            ),
        )
        elf = result.elf_file
        click.echo(f"SCHEMATIC compiled: {result.object_file}")
        if verbose and result.pass_output:
            click.echo(result.pass_output)

    elif mode == "none":
        click.echo("Mode 'none': no checkpoint insertion. Use --mode to select.")
        return
    else:
        raise click.UsageError(f"Unknown mode: {mode}")

    if elf:
        click.echo(f"ELF: {elf}")

    if not compile_only and elf and elf.exists():
        from .device.flash import flash_run_and_read

        click.echo("Flashing and reading output...")
        output_text = flash_run_and_read(tc, elf, timeout=30, symbols=[])
        click.echo(output_text)


# =========================================================================
# bench group
# =========================================================================

@main.group()
def bench() -> None:
    """Run benchmarks."""


@bench.command("milp")
@click.argument("benchmarks", nargs=-1)
@click.option("--cap", multiple=True, help="Capacitor sizes (e.g., 1uF 10uF).")
@click.option("--debug-counters", is_flag=True, help="Enable debug counters.")
@click.option("-o", "--output", type=click.Path(), help="Output CSV path.")
@click.option("-v", "--verbose", is_flag=True, help="Verbose output.")
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
@click.pass_context
def bench_milp_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    cap: tuple[str, ...],
    debug_counters: bool,
    output: str | None,
    verbose: bool,
    halt_mode: str,
    estimator_mode: str,
    energy_config: str | None,
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
        verbose=verbose,
        estimator_mode=estimator_mode,
        energy_config=Path(energy_config) if energy_config else None,
    )


@bench.command("rockclimb")
@click.argument("benchmarks", nargs=-1)
@click.option("--cap", multiple=True, help="Capacitor sizes (e.g., 1uF 10uF).")
@click.option("--debug-counters", is_flag=True, help="Enable debug counters.")
@click.option("-o", "--output", type=click.Path(), help="Output CSV path.")
@click.option("-v", "--verbose", is_flag=True, help="Verbose output.")
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
@click.pass_context
def bench_rockclimb_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    cap: tuple[str, ...],
    debug_counters: bool,
    output: str | None,
    verbose: bool,
    halt_mode: str,
    energy_config: str | None,
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
        verbose=verbose,
        energy_config=Path(energy_config) if energy_config else None,
    )


@bench.command("schematic")
@click.argument("benchmarks", nargs=-1)
@click.option("--cap", multiple=True, help="Capacitor sizes (e.g., 1uF 10uF).")
@click.option("--debug-counters", is_flag=True, help="Enable debug counters.")
@click.option("-o", "--output", type=click.Path(), help="Output CSV path.")
@click.option("-v", "--verbose", is_flag=True, help="Verbose output.")
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
@click.pass_context
def bench_schematic_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    cap: tuple[str, ...],
    debug_counters: bool,
    output: str | None,
    verbose: bool,
    halt_mode: str,
    energy_config: str | None,
    trace_config: str | None,
    estimator_mode: str,
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
        verbose=verbose,
        energy_config=Path(energy_config) if energy_config else None,
        trace_config=Path(trace_config) if trace_config else None,
        estimator_mode=estimator_mode,
    )


# =========================================================================
# verify group
# =========================================================================

@main.group()
def verify() -> None:
    """Verification commands."""


@verify.command("rockclimb")
@click.argument("benchmarks", nargs=-1)
@click.option(
    "--cap",
    default="1uF",
    help="Capacitor size for verification (default: 1uF).",
)
@click.option("--timeout", type=int, default=30, help="Serial timeout in seconds.")
@click.option("-v", "--verbose", is_flag=True, help="Show full compile/serial output.")
@click.option(
    "-e",
    "--energy-config",
    type=click.Path(exists=True),
    help="Override default energy config.",
)
@click.option(
    "-c",
    "--rockclimb-config",
    type=click.Path(exists=True),
    help="Override default RockClimb config.",
)
@click.option(
    "--halt-mode",
    type=click.Choice(["nop", "bor", "lpm4"]),
    default="nop",
    help="Halt mode for linked binary.",
)
@click.pass_context
def verify_rockclimb_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    cap: str,
    timeout: int,
    verbose: bool,
    energy_config: str | None,
    rockclimb_config: str | None,
    halt_mode: str,
) -> None:
    """Verify semantic correctness of RockClimb checkpoint insertion."""
    from .verify.rockclimb import verify_rockclimb

    success = verify_rockclimb(
        ctx.obj["env"],
        ctx.obj["tc"],
        benchmarks=list(benchmarks) if benchmarks else None,
        cap_size=cap,
        timeout=timeout,
        verbose=verbose,
        halt_mode=halt_mode,
        energy_config=Path(energy_config) if energy_config else None,
        rockclimb_config=Path(rockclimb_config) if rockclimb_config else None,
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


@device.command("read-nvm")
@click.argument("elf", type=click.Path(exists=True))
@click.argument("symbols", nargs=-1, required=True)
@click.pass_context
def device_read_nvm_cmd(
    ctx: click.Context,
    elf: str,
    symbols: tuple[str, ...],
) -> None:
    """Read NVM symbol values from a flashed device."""
    from .device.flash import flash_run_and_read

    tc = ctx.obj["tc"]
    elf_path = Path(elf)

    values = flash_run_and_read(tc, elf_path, 30, list(symbols))

    for sym, val in values.items():
        click.echo(f"{sym}={val}")


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

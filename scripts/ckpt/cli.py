"""Click-based CLI for the ckpt package.

Provides subcommands for compilation, benchmarking, verification,
analysis, and device interaction.
"""

from __future__ import annotations

import logging
import time
from pathlib import Path

import click

from .analysis.plot import ALGORITHMS, METRICS
from .bench.all import ALL_ALGORITHMS, DEFAULT_ALGORITHMS
from .compile.common import HALT_MODES
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

_saleae_timeout_option = click.option(
    "--timeout",
    type=click.FloatRange(min=0, min_open=True),
    default=200.0,
    show_default=True,
    help="Maximum wait in seconds for each Saleae capture.",
)


# ---------------------------------------------------------------------------
# Shared option declarations
# ---------------------------------------------------------------------------


def _add_options(*options):
    """Compose several click option decorators into one (applied in order)."""

    def decorator(fn):
        for option in reversed(options):
            fn = option(fn)
        return fn

    return decorator


def _energy_config_option(help_text: str):
    return click.option(
        "-e", "--energy-config", type=click.Path(exists=True), help=help_text
    )


def _cpu_freq_option(default: str):
    return click.option(
        "--cpu-freq",
        type=click.Choice(["1", "8", "16"]),
        default=default,
        help=f"CPU frequency in MHz (default: {default}).",
    )


def _clang_opt_level_option(default: int):
    return click.option(
        "-Oc", "clang_opt_level", type=int, default=default, help="Clang opt level."
    )


_cap_option = click.option(
    "--cap",
    help="Capacitor size (e.g. 1uF) — resolves to benchmarks/config_{cap}.json.",
)


def _expand_caps(values: tuple[str, ...]) -> tuple[str, ...]:
    """Split comma-separated --cap values; bare numbers get a uF suffix."""
    caps: list[str] = []
    for value in values:
        for token in value.split(","):
            token = token.strip()
            if not token:
                continue
            if token.replace(".", "", 1).isdigit():
                token += "uF"
            caps.append(token)
    return tuple(caps)


def _cap_callback(
    ctx: click.Context, param: click.Parameter, value: tuple[str, ...]
) -> tuple[str, ...]:
    return _expand_caps(value)


_cap_multi_option = click.option(
    "--cap",
    multiple=True,
    callback=_cap_callback,
    help="Capacitor sizes; repeat or comma-separate (e.g., 5uF,10uF or 5,10,50).",
)
_output_option = click.option("-o", "--output", type=click.Path())
_output_csv_option = click.option(
    "-o", "--output", "--csv", type=click.Path(), help="Output CSV path."
)
_compile_csv_option = click.option(
    "--csv",
    "csv_path",
    type=click.Path(),
    help="Also write a one-row stats CSV (compile-time fields only) to this path.",
)
_link_flag = click.option("--link", is_flag=True, help="Link with boot.S and runtime.")
_link_toggle = click.option(
    "--link/--no-link", default=True, help="Link with boot.S and runtime (default: on)."
)
_debug_flag = click.option("--debug", is_flag=True, help="Enable DEBUG output.")
_device_debug_flag = click.option(
    "--device-debug", is_flag=True, help="Enable device debug."
)
_device_debug_toggle = click.option(
    "--device-debug/--no-device-debug",
    default=True,
    help="Enable device debug (default: on).",
)
_opt_level_option = click.option(
    "-O", "opt_level", type=int, default=3, help="LLC opt level."
)
_extra_includes_option = click.option(
    "-I", "extra_includes", multiple=True, help="Extra include dirs."
)
_estimator_mode_option = click.option(
    "--estimator-mode",
    type=click.Choice(["assembly", "ir"]),
    default="assembly",
    help="Energy estimator mode.",
)
_save_temps_flag = click.option(
    "--save-temps", is_flag=True, help="Save intermediate files to output directory."
)
_accumulate_keys_option = click.option(
    "--accumulate-keys",
    type=click.Path(),
    help="Accumulate required energy keys to this file.",
)
_coarse_allocation_flag = click.option(
    "--coarse-allocation",
    is_flag=True,
    help="Use one MILP placement variable per eligible value instead of per-region placement.",
)
_milp_gap_option = click.option(
    "--milp-gap",
    type=float,
    default=0.0,
    help="MIP optimality gap (default: 0.0 = proven optimal).",
)
_max_unroll_option = click.option(
    "--max-unroll",
    type=click.IntRange(min=1),
    default=4,
    show_default=True,
    help="Maximum RockClimb partial unroll factor for the preprocess pass.",
)
_trace_config_option = click.option(
    "--trace-config",
    type=click.Path(exists=True),
    help="Override trace-collection config (default: config_10uF.json).",
)
_compile_halt_mode_option = click.option(
    "--halt-mode",
    type=click.Choice(HALT_MODES),
    default="swbor",
    help="Halt mode for linked binary (default: swbor).",
)
_bench_halt_mode_option = click.option(
    "--halt-mode",
    type=click.Choice(HALT_MODES),
    default="swbor",
    help="Halt mode for linked binary.",
)
# Verification uses BOR, which destroys modeled volatile state before recovery.
_verify_halt_mode_option = click.option(
    "--halt-mode",
    type=click.Choice(HALT_MODES),
    default="bor",
    help="Halt mode for linked binary (default: bor).",
)
_energy_override_option = _energy_config_option("Override default energy config.")


def _mhz_to_hz(cpu_freq: str) -> int:
    return int(cpu_freq) * 1_000_000


def _list_or_none(values: tuple[str, ...]) -> list[str] | None:
    return list(values) if values else None


def _path_or_none(value: str | None) -> Path | None:
    return Path(value) if value else None


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

    raise click.UsageError(f"Provide {option_name} or --cap.")


def _write_compile_csv(
    csv_path: str,
    *,
    algorithm: str,
    benchmark: str,
    capacitor: str,
    result,
) -> None:
    """Write a one-row CSV of compile-time stats for a single compilation.

    Emits the same columns ``ckpt bench <algorithm>`` would, minus the
    device-only runtime columns. *algorithm* selects the column schema
    (``schematicO3`` shares SCHEMATIC's schema).
    """
    import json

    from .bench.runner import write_compile_stats_csv
    from .output_parser import has_pass_statistics, load_stats_json, parse_pass_output

    if algorithm == "milp":
        from .bench.milp import CSV_HEADER, build_row
    elif algorithm == "rockclimb":
        from .bench.rockclimb import CSV_HEADER, build_row
    else:  # schematic / schematicO3 share the SCHEMATIC schema
        from .bench.schematic import CSV_HEADER, build_row

    status = "ok"
    stats = None
    if result.stats_json is not None and result.stats_json.is_file():
        data = json.loads(result.stats_json.read_text())
        stats, feasible, _ = load_stats_json(data)
        if not feasible:
            status = "infeasible"
    elif has_pass_statistics(result.pass_output):
        stats = parse_pass_output(result.pass_output)

    if stats is None:
        logger.warning("No pass statistics produced; stats CSV not written.")
        return

    write_compile_stats_csv(
        Path(csv_path),
        CSV_HEADER,
        build_row,
        benchmark=benchmark,
        capacitor=capacitor,
        status=status,
        stats=stats,
        profiling_time_ms=getattr(result, "profiling_time_ms", 0) or 0,
        pass_output=result.pass_output,
    )
    logger.info("Stats CSV: %s", csv_path)


def _handle_accumulate_keys(result, accumulate_keys: str | None) -> None:
    """Append the compile's required energy keys to the accumulate file.

    Reads the keys from the pass output, falling back to the stats JSON
    sidecar when the pass output carries no energy-parameter block.
    """
    if not accumulate_keys:
        return
    import json

    from .bench.runner import accumulate_keys_to_file, extract_energy_params

    ep = extract_energy_params(result.pass_output)
    if ep is not None:
        accumulate_keys_to_file(set(ep[0]), Path(accumulate_keys))
    elif result.stats_json is not None and result.stats_json.is_file():
        data = json.loads(result.stats_json.read_text())
        req = data.get("required_energy_keys", [])
        if req:
            accumulate_keys_to_file(set(req), Path(accumulate_keys))


@compile.command("milp")
@click.argument("input_c")
@_energy_config_option("Energy config (default: auto-detected by estimator mode).")
@click.option(
    "-m",
    "--milp-config",
    type=click.Path(exists=True),
    help="MILP config (alternative to --cap).",
)
@_cap_option
@_output_option
@_compile_csv_option
@_link_flag
@_estimator_mode_option
@_debug_flag
@_device_debug_flag
@_compile_halt_mode_option
@_opt_level_option
@_clang_opt_level_option(3)
@_extra_includes_option
@_cpu_freq_option("16")
@_save_temps_flag
@_milp_gap_option
@click.option(
    "--milp-log-file", type=click.Path(), default="", help="Gurobi log file path."
)
@_coarse_allocation_flag
@_accumulate_keys_option
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
    csv_path: str | None,
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
        env,
        "milp",
        milp_config,
        cap,
        "-m/--milp-config",
    )

    energy_config_path: Path
    if energy_config is not None:
        energy_config_path = Path(energy_config)
    elif estimator_mode == "ir":
        energy_config_path = (
            env.project_dir / "benchmarks" / "sample_energy_config_ir.json"
        )
    else:
        energy_config_path = default_energy_config(env, "milp")

    cpu_freq_hz = _mhz_to_hz(cpu_freq)

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

    _handle_accumulate_keys(result, accumulate_keys)

    logger.info("Object: %s", result.object_file)
    logger.info("Assembly: %s", result.assembly_file)
    if result.elf_file:
        logger.info("ELF: %s", result.elf_file)
    logger.debug("Pass output:\n%s", result.pass_output)

    if csv_path:
        _write_compile_csv(
            csv_path,
            algorithm="milp",
            benchmark=input_path.stem,
            capacitor=cap or "",
            result=result,
        )


@compile.command("rockclimb")
@click.argument("input_c")
@_energy_config_option("Energy config (default: auto-detected).")
@click.option(
    "-c",
    "--rockclimb-config",
    type=click.Path(exists=True),
    help="RockClimb config (alternative to --cap).",
)
@_cap_option
@_output_option
@_link_flag
@_device_debug_flag
@_compile_halt_mode_option
@_opt_level_option
@_clang_opt_level_option(3)
@click.option(
    "--no-precomputed-energy",
    is_flag=True,
    help="Disable assembly-based pre-computed BB energy; use MIR-level estimation instead.",
)
@_cpu_freq_option("16")
@_max_unroll_option
@_save_temps_flag
@_accumulate_keys_option
@_compile_csv_option
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
    max_unroll: int,
    save_temps: bool,
    accumulate_keys: str | None,
    csv_path: str | None,
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
        env,
        "rockclimb",
        rockclimb_config,
        cap,
        "-c/--rockclimb-config",
    )

    energy_config_path: Path
    if energy_config is not None:
        energy_config_path = Path(energy_config)
    else:
        energy_config_path = default_energy_config(env, "rockclimb")

    cpu_freq_hz = _mhz_to_hz(cpu_freq)

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
            max_unroll=max_unroll,
            save_temps=save_temps,
            linker_script=None,
        ),
    )

    _handle_accumulate_keys(result, accumulate_keys)

    logger.info("Assembly: %s", result.assembly_file)
    if result.elf_file:
        logger.info("ELF: %s", result.elf_file)
    logger.debug("Pass output:\n%s", result.pass_output)

    if csv_path:
        _write_compile_csv(
            csv_path,
            algorithm="rockclimb",
            benchmark=input_path.stem,
            capacitor=cap or "",
            result=result,
        )


def _compile_schematic_impl(
    ctx: click.Context,
    *,
    algorithm_label: str,
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
    csv_path: str | None,
) -> None:
    from .bench.config import default_energy_config
    from .compile.schematic import SchematicCompileOptions, compile_schematic

    env = ctx.obj["env"]

    input_path = _resolve_input(env, input_c)
    output_path = Path(output) if output else Path("build") / input_path.stem
    cpu_freq_hz = _mhz_to_hz(cpu_freq)

    schematic_config_path: Path | None
    if not trace_only:
        schematic_config_path = _resolve_algorithm_config(
            env,
            algorithm_label,
            schematic_config,
            cap,
            "-s/--schematic-config or --cap",
        )
    else:
        schematic_config_path = Path(schematic_config) if schematic_config else None

    energy_config_path: Path
    if energy_config is not None:
        energy_config_path = Path(energy_config)
    elif estimator_mode == "ir":
        energy_config_path = (
            env.project_dir / "benchmarks" / "sample_energy_config_ir.json"
        )
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
            trace_file=_path_or_none(trace_file),
            save_temps=save_temps,
            linker_script=None,
        ),
    )

    _handle_accumulate_keys(result, accumulate_keys)

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

    if csv_path:
        _write_compile_csv(
            csv_path,
            algorithm=algorithm_label,
            benchmark=input_path.stem,
            capacitor=cap or "",
            result=result,
        )


def _compile_schematic_options(clang_opt_default: int):
    """Full option stack shared by the schematic and schematicO3 commands."""
    return _add_options(
        _energy_config_option(
            "Energy config (default: auto-detected by estimator mode)."
        ),
        click.option(
            "-s",
            "--schematic-config",
            type=click.Path(exists=True),
            help="SCHEMATIC config (alternative to --cap).",
        ),
        _cap_option,
        _output_option,
        _link_flag,
        _debug_flag,
        _device_debug_flag,
        _compile_halt_mode_option,
        click.option(
            "--trace-file",
            type=click.Path(exists=True),
            help="Pre-collected trace JSON.",
        ),
        click.option(
            "--trace-only", is_flag=True, help="Only collect trace, skip insertion."
        ),
        _opt_level_option,
        _clang_opt_level_option(clang_opt_default),
        _extra_includes_option,
        _estimator_mode_option,
        _cpu_freq_option("16"),
        _save_temps_flag,
        _accumulate_keys_option,
        _compile_csv_option,
    )


@compile.command("schematic")
@click.argument("input_c")
@_compile_schematic_options(clang_opt_default=0)
@click.pass_context
def compile_schematic_cmd(ctx: click.Context, **kwargs) -> None:
    """Run the SCHEMATIC trace-based compilation pipeline.

    INPUT_C can be a benchmark name (e.g. "crc") or a path to a C file.
    """
    _compile_schematic_impl(ctx, algorithm_label="schematic", **kwargs)


@compile.command("schematicO3")
@click.argument("input_c")
@_compile_schematic_options(clang_opt_default=3)
@click.pass_context
def compile_schematic_o3_cmd(ctx: click.Context, **kwargs) -> None:
    """Run the SCHEMATIC-O3 trace-based compilation pipeline (clang -O3).

    INPUT_C can be a benchmark name (e.g. "crc") or a path to a C file.
    """
    _compile_schematic_impl(ctx, algorithm_label="schematicO3", **kwargs)


@compile.command("uninstrumented")
@click.argument("input_c")
@_output_option
@_link_toggle
@_device_debug_toggle
@_opt_level_option
@_clang_opt_level_option(3)
@_extra_includes_option
@_cpu_freq_option("16")
@_compile_csv_option
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
    csv_path: str | None,
) -> None:
    """Compile without checkpoint insertion (baseline).

    INPUT_C can be a benchmark name (e.g. "crc") or a path to a C file.
    """
    import time

    from .compile.uninstrumented import (
        UninstrumentedCompileOptions,
        compile_uninstrumented,
    )

    input_path = _resolve_input(ctx.obj["env"], input_c)
    output_path = Path(output) if output else Path("build") / input_path.stem
    cpu_freq_hz = _mhz_to_hz(cpu_freq)

    t0 = time.monotonic()
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
    compilation_time_ms = int((time.monotonic() - t0) * 1000)

    logger.info("Object: %s", result.object_file)
    logger.info("Assembly: %s", result.assembly_file)
    if result.elf_file:
        logger.info("ELF: %s", result.elf_file)

    if csv_path:
        import csv

        from .bench.runner import static_csv_header
        from .bench.uninstrumented import CSV_HEADER as _UNINST_HEADER

        header = static_csv_header(_UNINST_HEADER)
        out = Path(csv_path)
        out.parent.mkdir(parents=True, exist_ok=True)
        with open(out, "w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(header)
            writer.writerow([input_path.stem, "ok", str(compilation_time_ms)])
        logger.info("Stats CSV: %s", csv_path)


@compile.command("chunked")
@click.argument("input_c")
@_energy_config_option("Energy estimator config. Defaults to assembly params.")
@click.option(
    "-m",
    "--milp-config",
    type=click.Path(exists=True),
    help="MILP config JSON (for chunk size K).",
)
@_cap_option
@_output_option
@_link_toggle
@_device_debug_toggle
@_opt_level_option
@_clang_opt_level_option(3)
@_extra_includes_option
@_cpu_freq_option("16")
@click.pass_context
def compile_chunked_cmd(
    ctx: click.Context,
    input_c: str,
    energy_config: str | None,
    milp_config: str | None,
    cap: str | None,
    output: str | None,
    link: bool,
    device_debug: bool,
    opt_level: int,
    clang_opt_level: int,
    extra_includes: tuple[str, ...],
    cpu_freq: str,
) -> None:
    """Compile with loop chunking but no checkpoint insertion.

    Runs the MILP preprocessing phases (loop canonicalization +
    strip-mining + reclamp), then compiles like the uninstrumented
    baseline. Isolates the control-flow overhead of loop chunking.

    INPUT_C can be a benchmark name (e.g. "crc") or a path to a C file.
    """
    from .bench.config import default_energy_config
    from .compile.chunked import ChunkedCompileOptions, compile_chunked

    env = ctx.obj["env"]

    input_path = _resolve_input(env, input_c)
    output_path = Path(output) if output else Path("build") / input_path.stem

    milp_config_path = _resolve_algorithm_config(
        env,
        "milp",
        milp_config,
        cap,
        "-m/--milp-config",
    )

    energy_config_path = (
        Path(energy_config)
        if energy_config is not None
        else default_energy_config(env, "milp")
    )

    result = compile_chunked(
        ctx.obj["tc"],
        env,
        ChunkedCompileOptions(
            input_c=input_path,
            energy_config=energy_config_path,
            milp_config=milp_config_path,
            output=output_path,
            pass_log_level=ctx.obj["pass_log_level"],
            device_debug=device_debug,
            cpu_freq=_mhz_to_hz(cpu_freq),
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
@_cap_multi_option
@_device_debug_toggle
@_output_csv_option
@_bench_halt_mode_option
@_estimator_mode_option
@_energy_override_option
@_cpu_freq_option("16")
@_coarse_allocation_flag
@_milp_gap_option
@_accumulate_keys_option
@_saleae_timeout_option
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
    milp_gap: float,
    accumulate_keys: str | None,
    timeout: float,
) -> None:
    """Run MILP benchmarks across programs and capacitor sizes."""
    from .bench.milp import run_milp_benchmarks

    run_milp_benchmarks(
        ctx.obj["env"],
        ctx.obj["tc"],
        benchmarks=_list_or_none(benchmarks),
        caps=_list_or_none(cap),
        device_debug=device_debug,
        capture_timeout_seconds=timeout,
        halt_mode=halt_mode,
        output_csv=_path_or_none(output),
        estimator_mode=estimator_mode,
        energy_config=_path_or_none(energy_config),
        cpu_freq=_mhz_to_hz(cpu_freq),
        coarse_allocation=coarse_allocation,
        milp_gap=milp_gap,
        pass_log_level=ctx.obj["pass_log_level"],
        accumulate_keys_file=_path_or_none(accumulate_keys),
    )


@bench.command("rockclimb")
@click.argument("benchmarks", nargs=-1)
@_cap_multi_option
@_device_debug_toggle
@_output_csv_option
@_bench_halt_mode_option
@_energy_override_option
@_cpu_freq_option("16")
@_max_unroll_option
@_accumulate_keys_option
@_saleae_timeout_option
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
    max_unroll: int,
    accumulate_keys: str | None,
    timeout: float,
) -> None:
    """Run RockClimb benchmarks across programs and capacitor sizes."""
    from .bench.rockclimb import run_rockclimb_benchmarks

    run_rockclimb_benchmarks(
        ctx.obj["env"],
        ctx.obj["tc"],
        benchmarks=_list_or_none(benchmarks),
        caps=_list_or_none(cap),
        device_debug=device_debug,
        capture_timeout_seconds=timeout,
        halt_mode=halt_mode,
        output_csv=_path_or_none(output),
        energy_config=_path_or_none(energy_config),
        cpu_freq=_mhz_to_hz(cpu_freq),
        max_unroll=max_unroll,
        pass_log_level=ctx.obj["pass_log_level"],
        accumulate_keys_file=_path_or_none(accumulate_keys),
    )


_bench_schematic_options = _add_options(
    _cap_multi_option,
    _device_debug_toggle,
    _output_csv_option,
    _bench_halt_mode_option,
    _energy_override_option,
    _trace_config_option,
    _estimator_mode_option,
    _cpu_freq_option("16"),
    _accumulate_keys_option,
    _saleae_timeout_option,
)


def _bench_schematic_impl(
    ctx: click.Context,
    *,
    algorithm_label: str,
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
    timeout: float,
) -> None:
    from .bench.schematic import run_schematic_benchmarks
    from .compile.schematic import CLANG_OPT_LEVEL_BY_LABEL

    run_schematic_benchmarks(
        ctx.obj["env"],
        ctx.obj["tc"],
        benchmarks=_list_or_none(benchmarks),
        caps=_list_or_none(cap),
        device_debug=device_debug,
        capture_timeout_seconds=timeout,
        halt_mode=halt_mode,
        output_csv=_path_or_none(output),
        energy_config=_path_or_none(energy_config),
        trace_config=_path_or_none(trace_config),
        estimator_mode=estimator_mode,
        cpu_freq=_mhz_to_hz(cpu_freq),
        clang_opt_level=CLANG_OPT_LEVEL_BY_LABEL[algorithm_label],
        pass_log_level=ctx.obj["pass_log_level"],
        algorithm_label=algorithm_label,
        accumulate_keys_file=_path_or_none(accumulate_keys),
    )


@bench.command("schematic")
@click.argument("benchmarks", nargs=-1)
@_bench_schematic_options
@click.pass_context
def bench_schematic_cmd(ctx: click.Context, **kwargs) -> None:
    """Run SCHEMATIC benchmarks across programs and capacitor sizes."""
    _bench_schematic_impl(ctx, algorithm_label="schematic", **kwargs)


@bench.command("schematicO3")
@click.argument("benchmarks", nargs=-1)
@_bench_schematic_options
@click.pass_context
def bench_schematic_o3_cmd(ctx: click.Context, **kwargs) -> None:
    """Run SCHEMATIC-O3 benchmarks across programs and capacitor sizes."""
    _bench_schematic_impl(ctx, algorithm_label="schematicO3", **kwargs)


@bench.command("uninstrumented")
@click.argument("benchmarks", nargs=-1)
@_output_csv_option
@_cpu_freq_option("16")
@_saleae_timeout_option
@click.pass_context
def bench_uninstrumented_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    output: str | None,
    cpu_freq: str,
    timeout: float,
) -> None:
    """Run uninstrumented baselines and measure execution time."""
    _bench_uninstrumented_impl(
        ctx, benchmarks, output, cpu_freq, timeout, algorithm_label="uninstrumented"
    )


def _bench_uninstrumented_impl(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    output: str | None,
    cpu_freq: str,
    timeout: float,
    algorithm_label: str,
) -> None:
    from .bench.uninstrumented import run_uninstrumented_benchmarks
    from .compile.uninstrumented import OPT_LEVELS_BY_LABEL

    clang_opt_level, opt_level = OPT_LEVELS_BY_LABEL[algorithm_label]
    run_uninstrumented_benchmarks(
        ctx.obj["env"],
        ctx.obj["tc"],
        benchmarks=_list_or_none(benchmarks),
        output_csv=_path_or_none(output),
        capture_timeout_seconds=timeout,
        cpu_freq=_mhz_to_hz(cpu_freq),
        algorithm_label=algorithm_label,
        clang_opt_level=clang_opt_level,
        opt_level=opt_level,
    )


@bench.command("uninstrumentedO0")
@click.argument("benchmarks", nargs=-1)
@_output_csv_option
@_cpu_freq_option("16")
@_saleae_timeout_option
@click.pass_context
def bench_uninstrumented_o0_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    output: str | None,
    cpu_freq: str,
    timeout: float,
) -> None:
    """Run uninstrumented baselines with O0 frontend IR and O3 backend."""
    _bench_uninstrumented_impl(
        ctx, benchmarks, output, cpu_freq, timeout, algorithm_label="uninstrumentedO0"
    )


@bench.command("chunked")
@click.argument("benchmarks", nargs=-1)
@_cap_multi_option
@_output_csv_option
@_energy_override_option
@_cpu_freq_option("16")
@_saleae_timeout_option
@click.pass_context
def bench_chunked_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    cap: tuple[str, ...],
    output: str | None,
    energy_config: str | None,
    cpu_freq: str,
    timeout: float,
) -> None:
    """Run chunking-only baselines across programs and capacitor sizes."""
    from .bench.chunked import run_chunked_benchmarks
    from .compile.chunked import OPT_LEVELS

    clang_opt_level, opt_level = OPT_LEVELS
    run_chunked_benchmarks(
        ctx.obj["env"],
        ctx.obj["tc"],
        benchmarks=_list_or_none(benchmarks),
        caps=_list_or_none(cap),
        output_csv=_path_or_none(output),
        energy_config=_path_or_none(energy_config),
        capture_timeout_seconds=timeout,
        cpu_freq=_mhz_to_hz(cpu_freq),
        clang_opt_level=clang_opt_level,
        opt_level=opt_level,
        pass_log_level=ctx.obj["pass_log_level"],
    )


def _algorithms_callback(
    ctx: click.Context, param: click.Parameter, value: str
) -> list[str]:
    """Parse and validate the comma-separated --algorithms list."""
    algorithms = [name.strip() for name in value.split(",") if name.strip()]
    unknown = [name for name in algorithms if name not in ALL_ALGORITHMS]
    if unknown:
        raise click.BadParameter(
            f"unknown algorithm(s): {', '.join(unknown)}. "
            f"Choose from: {', '.join(ALL_ALGORITHMS)}"
        )
    if not algorithms:
        raise click.BadParameter("at least one algorithm is required")
    return algorithms


def _default_result_dir() -> str:
    return str(Path("result") / time.strftime("%Y%m%d-%H%M%S"))


@bench.command("all")
@click.argument("benchmarks", nargs=-1)
@_cap_multi_option
@click.option(
    "-d",
    "--result-dir",
    type=click.Path(),
    default=None,
    help="Directory for raw CSVs and plots/ (default: result/<timestamp>).",
)
@click.option(
    "--algorithms",
    callback=_algorithms_callback,
    default=",".join(DEFAULT_ALGORITHMS),
    show_default=True,
    help="Comma-separated steps to run; also accepts uninstrumentedO0 and chunked.",
)
@_bench_halt_mode_option
@_estimator_mode_option
@_energy_override_option
@_cpu_freq_option("16")
@_coarse_allocation_flag
@_milp_gap_option
@_max_unroll_option
@click.option(
    "--skip-existing",
    is_flag=True,
    help="Skip steps whose CSV already exists (resume an interrupted run).",
)
@click.option(
    "--plot/--no-plot", default=True, help="Plot after benchmarking (default: on)."
)
@click.option(
    "--plot-config",
    type=click.Path(exists=True),
    help="Plot config for plot_results.R (default: scripts/plot_config.json).",
)
@_saleae_timeout_option
@click.pass_context
def bench_all_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    cap: tuple[str, ...],
    result_dir: str | None,
    algorithms: list[str],
    halt_mode: str,
    estimator_mode: str,
    energy_config: str | None,
    cpu_freq: str,
    coarse_allocation: bool,
    milp_gap: float,
    max_unroll: int,
    skip_existing: bool,
    plot: bool,
    plot_config: str | None,
    timeout: float,
) -> None:
    """Run every algorithm with and without device-debug, then plot."""
    from .bench.all import all_ok, format_summary, run_bench_all

    out_dir = Path(result_dir) if result_dir else Path(_default_result_dir())

    outcomes = run_bench_all(
        ctx.obj["env"],
        ctx.obj["tc"],
        benchmarks=_list_or_none(benchmarks),
        caps=_list_or_none(cap),
        result_dir=out_dir,
        algorithms=algorithms,
        halt_mode=halt_mode,
        estimator_mode=estimator_mode,
        energy_config=_path_or_none(energy_config),
        cpu_freq=_mhz_to_hz(cpu_freq),
        coarse_allocation=coarse_allocation,
        milp_gap=milp_gap,
        max_unroll=max_unroll,
        capture_timeout_seconds=timeout,
        pass_log_level=ctx.obj["pass_log_level"],
        skip_existing=skip_existing,
        plot=plot,
        plot_config=_path_or_none(plot_config),
    )

    click.echo(format_summary(outcomes, out_dir))
    if not all_ok(outcomes):
        raise SystemExit(1)


# =========================================================================
# intermittent group
# =========================================================================


def _trace_callback(
    ctx: click.Context, param: click.Parameter, value: tuple[str, ...]
) -> tuple[str, ...]:
    """Split comma-separated --trace values."""
    specs: list[str] = []
    for entry in value:
        for token in entry.split(","):
            token = token.strip()
            if token:
                specs.append(token)
    return tuple(specs)


_trace_option = click.option(
    "--trace",
    required=True,
    multiple=True,
    callback=_trace_callback,
    help=(
        "Power trace to replay: a CSV path or a name in benchmarks/traces/ "
        "(e.g. 1); repeat or comma-separate."
    ),
)

_intermittent_common_options = _add_options(
    click.argument("benchmarks", nargs=-1),
    _trace_option,
    _cap_multi_option,
    _device_debug_toggle,
    _cpu_freq_option("16"),
    _output_csv_option,
)


def _run_intermittent(
    ctx: click.Context,
    algorithm: str,
    *,
    benchmarks: tuple[str, ...],
    trace: tuple[str, ...],
    cap: tuple[str, ...],
    device_debug: bool,
    estimator_mode: str,
    cpu_freq: str,
    output: str | None,
    max_unroll: int | None,
) -> None:
    from .intermittent.runner import run_intermittent_benchmarks

    run_intermittent_benchmarks(
        ctx.obj["env"],
        ctx.obj["tc"],
        algorithm=algorithm,
        benchmarks=_list_or_none(benchmarks),
        caps=_list_or_none(cap) or ["board"],
        trace_specs=list(trace),
        output_csv=_path_or_none(output),
        device_debug=device_debug,
        estimator_mode=estimator_mode,
        cpu_freq=_mhz_to_hz(cpu_freq),
        max_unroll=max_unroll,
        pass_log_level=ctx.obj["pass_log_level"],
    )


@main.group()
def intermittent() -> None:
    """Run benchmarks powered by a replayed energy-harvesting trace."""


@intermittent.command("milp")
@_intermittent_common_options
@_estimator_mode_option
@click.pass_context
def intermittent_milp_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    trace: tuple[str, ...],
    cap: tuple[str, ...],
    device_debug: bool,
    cpu_freq: str,
    output: str | None,
    estimator_mode: str,
) -> None:
    """Run MILP benchmarks under replayed intermittent power."""
    _run_intermittent(
        ctx,
        "milp",
        benchmarks=benchmarks,
        trace=trace,
        cap=cap,
        device_debug=device_debug,
        estimator_mode=estimator_mode,
        cpu_freq=cpu_freq,
        output=output,
        max_unroll=None,
    )


@intermittent.command("rockclimb")
@_intermittent_common_options
@_max_unroll_option
@click.pass_context
def intermittent_rockclimb_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    trace: tuple[str, ...],
    cap: tuple[str, ...],
    device_debug: bool,
    cpu_freq: str,
    output: str | None,
    max_unroll: int,
) -> None:
    """Run RockClimb benchmarks under replayed intermittent power."""
    _run_intermittent(
        ctx,
        "rockclimb",
        benchmarks=benchmarks,
        trace=trace,
        cap=cap,
        device_debug=device_debug,
        estimator_mode="assembly",
        cpu_freq=cpu_freq,
        output=output,
        max_unroll=max_unroll,
    )


@intermittent.command("schematic")
@_intermittent_common_options
@_estimator_mode_option
@click.pass_context
def intermittent_schematic_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    trace: tuple[str, ...],
    cap: tuple[str, ...],
    device_debug: bool,
    cpu_freq: str,
    output: str | None,
    estimator_mode: str,
) -> None:
    """Run SCHEMATIC benchmarks under replayed intermittent power."""
    _run_intermittent(
        ctx,
        "schematic",
        benchmarks=benchmarks,
        trace=trace,
        cap=cap,
        device_debug=device_debug,
        estimator_mode=estimator_mode,
        cpu_freq=cpu_freq,
        output=output,
        max_unroll=None,
    )


# =========================================================================
# verify group
# =========================================================================


@main.group()
def verify() -> None:
    """Verification commands."""


@verify.command("rockclimb")
@click.argument("benchmarks", nargs=-1)
@_cap_multi_option
@_energy_override_option
@_verify_halt_mode_option
@_cpu_freq_option("16")
@_saleae_timeout_option
@click.pass_context
def verify_rockclimb_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    cap: tuple[str, ...],
    energy_config: str | None,
    halt_mode: str,
    cpu_freq: str,
    timeout: float,
) -> None:
    """Verify semantic correctness of RockClimb checkpoint insertion."""
    from .verify.common import all_ok
    from .verify.rockclimb import verify_rockclimb

    results = verify_rockclimb(
        ctx.obj["env"],
        ctx.obj["tc"],
        benchmarks=_list_or_none(benchmarks),
        caps=_list_or_none(cap),
        halt_mode=halt_mode,
        capture_timeout_seconds=timeout,
        energy_config=_path_or_none(energy_config),
        cpu_freq=_mhz_to_hz(cpu_freq),
        pass_log_level=ctx.obj["pass_log_level"],
    )
    if not all_ok(results):
        raise SystemExit(1)


@verify.command("milp")
@click.argument("benchmarks", nargs=-1)
@_cap_multi_option
@_energy_override_option
@_verify_halt_mode_option
@_estimator_mode_option
@_cpu_freq_option("16")
@_coarse_allocation_flag
@_saleae_timeout_option
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
    timeout: float,
) -> None:
    """Verify semantic correctness of MILP checkpoint insertion."""
    from .verify.common import all_ok
    from .verify.milp import verify_milp

    results = verify_milp(
        ctx.obj["env"],
        ctx.obj["tc"],
        benchmarks=_list_or_none(benchmarks),
        caps=_list_or_none(cap),
        halt_mode=halt_mode,
        capture_timeout_seconds=timeout,
        energy_config=_path_or_none(energy_config),
        estimator_mode=estimator_mode,
        cpu_freq=_mhz_to_hz(cpu_freq),
        coarse_allocation=coarse_allocation,
        pass_log_level=ctx.obj["pass_log_level"],
    )
    if not all_ok(results):
        raise SystemExit(1)


_verify_schematic_options = _add_options(
    _cap_multi_option,
    _energy_override_option,
    _verify_halt_mode_option,
    _estimator_mode_option,
    _cpu_freq_option("16"),
    _saleae_timeout_option,
)


def _verify_schematic_impl(
    ctx: click.Context,
    *,
    algorithm_label: str,
    benchmarks: tuple[str, ...],
    cap: tuple[str, ...],
    energy_config: str | None,
    halt_mode: str,
    estimator_mode: str,
    cpu_freq: str,
    timeout: float,
) -> None:
    from .verify.common import all_ok
    from .verify.schematic import verify_schematic

    results = verify_schematic(
        ctx.obj["env"],
        ctx.obj["tc"],
        benchmarks=_list_or_none(benchmarks),
        caps=_list_or_none(cap),
        halt_mode=halt_mode,
        capture_timeout_seconds=timeout,
        energy_config=_path_or_none(energy_config),
        estimator_mode=estimator_mode,
        cpu_freq=_mhz_to_hz(cpu_freq),
        pass_log_level=ctx.obj["pass_log_level"],
        algorithm_label=algorithm_label,
    )
    if not all_ok(results):
        raise SystemExit(1)


@verify.command("schematic")
@click.argument("benchmarks", nargs=-1)
@_verify_schematic_options
@click.pass_context
def verify_schematic_cmd(ctx: click.Context, **kwargs) -> None:
    """Verify semantic correctness of SCHEMATIC checkpoint insertion."""
    _verify_schematic_impl(ctx, algorithm_label="schematic", **kwargs)


@verify.command("schematicO3")
@click.argument("benchmarks", nargs=-1)
@_verify_schematic_options
@click.pass_context
def verify_schematic_o3_cmd(ctx: click.Context, **kwargs) -> None:
    """Verify semantic correctness of SCHEMATIC-O3 checkpoint insertion."""
    _verify_schematic_impl(ctx, algorithm_label="schematicO3", **kwargs)


@verify.command("all")
@click.argument("benchmarks", nargs=-1)
@_cap_multi_option
@_energy_override_option
@_verify_halt_mode_option
@_estimator_mode_option
@_cpu_freq_option("16")
@_saleae_timeout_option
@click.option(
    "-o",
    "--output",
    type=click.Path(),
    help="Also write the final report to this file.",
)
@click.pass_context
def verify_all_cmd(
    ctx: click.Context,
    benchmarks: tuple[str, ...],
    cap: tuple[str, ...],
    energy_config: str | None,
    halt_mode: str,
    estimator_mode: str,
    cpu_freq: str,
    timeout: float,
    output: str | None,
) -> None:
    """Verify milp, rockclimb, schematic, and schematicO3 sequentially."""
    from .verify.all import format_report, verify_all
    from .verify.common import all_ok

    results = verify_all(
        ctx.obj["env"],
        ctx.obj["tc"],
        benchmarks=_list_or_none(benchmarks),
        caps=_list_or_none(cap),
        halt_mode=halt_mode,
        energy_config=_path_or_none(energy_config),
        estimator_mode=estimator_mode,
        cpu_freq=_mhz_to_hz(cpu_freq),
        capture_timeout_seconds=timeout,
        pass_log_level=ctx.obj["pass_log_level"],
    )

    report = format_report(results, halt_mode)
    click.echo(report)
    if output:
        Path(output).write_text(report + "\n")
        logger.info("Report written to %s", output)

    if not all(all_ok(algo_results) for algo_results in results.values()):
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


@analyze.command("milp-coarse")
@click.option(
    "--baseline-csv",
    type=click.Path(exists=True),
    default="results/milp_debug.csv",
    show_default=True,
    help="Baseline MILP debug CSV.",
)
@click.option(
    "--coarse-csv",
    type=click.Path(exists=True),
    default="results/milp_coarse_debug.csv",
    show_default=True,
    help="Coarse-allocation MILP debug CSV.",
)
@click.option(
    "-o",
    "--output",
    type=click.Path(),
    help="Optional CSV output path for the summary table.",
)
def analyze_milp_coarse_cmd(
    baseline_csv: str,
    coarse_csv: str,
    output: str | None,
) -> None:
    """Compare baseline and coarse-allocation MILP result CSVs."""
    from .analysis.milp_coarse import (
        format_milp_coarse_summary,
        summarize_milp_coarse_allocation,
        write_milp_coarse_summary_csv,
    )

    summary_rows = summarize_milp_coarse_allocation(
        Path(baseline_csv),
        Path(coarse_csv),
    )
    click.echo(format_milp_coarse_summary(summary_rows))

    if output is not None:
        write_milp_coarse_summary_csv(summary_rows, Path(output))


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
        algorithms=_list_or_none(algorithms),
        benchmarks=_list_or_none(benchmarks),
        capacitors=_list_or_none(capacitors),
        normalize=normalize,
        output_file=_path_or_none(output),
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

    output = read_serial_output(
        device=None,
        baud=9600,
        timeout=timeout,
        end_marker=end_marker,
        reset_cmd=None,
    )
    click.echo(output)

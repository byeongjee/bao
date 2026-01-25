"""Command-line interface for CFG analyzer."""

from pathlib import Path

import click
from rich.console import Console
from rich.table import Table


def _get_default_output_dir() -> str:
    """Get default output directory (tmp/cfg_output under project root)."""
    # Try to find project root by looking for pyproject.toml or .git
    current = Path.cwd()
    for parent in [current] + list(current.parents):
        if (parent / ".git").exists() or (parent / "passes").exists():
            return str(parent / "tmp" / "cfg_output")
    return "./tmp/cfg_output"

from .annotations import get_all_loop_bounds, parse_loop_bounds_from_source
from .cfg import FunctionCFG
from .compiler import compile_to_ir, find_llvm_tools, generate_dot_files
from .debug_info import parse_debug_info
from .loop_mapper import find_back_edges, map_loops_to_bounds
from .parser import extract_function_name, extract_node_to_block_mapping, parse_dot_file
from .path_counter import PathCountStrategy, count_paths, count_bounded_loop_paths_per_loop
from .visualizer import visualize_cfg

console = Console()


@click.group()
@click.version_option(version="0.1.0")
def main():
    """CFG Analyzer - Analyze control flow graphs of C programs."""
    pass


@main.command()
@click.argument("c_file", type=click.Path(exists=True, path_type=Path))
@click.option(
    "--output-dir",
    "-o",
    type=click.Path(path_type=Path),
    default=None,
    help="Output directory for generated files (default: tmp/cfg_output)",
)
@click.option(
    "--visualize/--no-visualize",
    default=True,
    help="Generate CFG images",
)
@click.option(
    "--format",
    type=click.Choice(["png", "svg", "pdf"]),
    default="png",
    help="Output format for visualizations",
)
@click.option(
    "--path-strategy",
    type=click.Choice(["acyclic", "bounded", "collapsed"]),
    default="bounded",
    help="Strategy for counting paths with loops",
)
@click.option(
    "--max-loop-iter",
    type=int,
    default=2,
    help="Max loop iterations for 'bounded' strategy",
)
@click.option(
    "-I",
    "--include",
    "include_paths",
    multiple=True,
    type=click.Path(exists=True, path_type=Path),
    help="Add include path for compilation (can be used multiple times)",
)
@click.option(
    "--embedded",
    is_flag=True,
    default=False,
    help="Enable embedded/microcontroller code compatibility (disables section attributes)",
)
@click.option(
    "--auto-bounds",
    is_flag=True,
    default=False,
    help="Parse loop bounds from source annotations (LOOP_BOUND macro or comments)",
)
@click.option(
    "--per-loop-bounds",
    is_flag=True,
    default=False,
    help="Use per-loop bounds from LOOP_BOUND annotations (maps each annotation to its loop)",
)
def analyze(
    c_file: Path,
    output_dir: Path,
    visualize: bool,
    format: str,
    path_strategy: str,
    max_loop_iter: int,
    include_paths: tuple[Path, ...],
    embedded: bool,
    auto_bounds: bool,
    per_loop_bounds: bool,
):
    """Analyze CFG of a C file."""
    if output_dir is None:
        output_dir = Path(_get_default_output_dir())
    else:
        output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    # Map string to enum
    strategy_map = {
        "acyclic": PathCountStrategy.ACYCLIC,
        "bounded": PathCountStrategy.BOUNDED,
        "collapsed": PathCountStrategy.COLLAPSED,
    }
    strategy = strategy_map[path_strategy]

    # Prepare compilation options
    inc_paths = list(include_paths) if include_paths else None
    extra_flags = None
    if embedded:
        # Disable section attributes that don't work on host OS
        # But preserve annotate attributes for loop bounds
        extra_flags = ["-D__section__(x)=", "-Dsection(x)="]

    # Step 1: Compile to LLVM IR
    # Include debug info if per-loop bounds requested
    include_debug = per_loop_bounds

    with console.status("[bold green]Compiling to LLVM IR..."):
        result = compile_to_ir(
            c_file, output_dir, inc_paths, extra_flags,
            include_debug_info=include_debug
        )
        if not result.success:
            console.print(f"[red]Compilation failed:[/red]\n{result.stderr}")
            raise SystemExit(1)

    console.print(f"[green]Generated LLVM IR:[/green] {result.ir_path}")

    # Parse debug info for per-loop bounds
    debug_info = None
    if per_loop_bounds:
        debug_info = parse_debug_info(result.ir_path)
        if debug_info.annotation_line_to_bound:
            console.print("[green]Found loop bound annotations:[/green]")
            for line, bound in sorted(debug_info.annotation_line_to_bound.items()):
                console.print(f"  Line {line}: bound = {bound}")
        else:
            console.print("[yellow]No loop bound annotations found in IR[/yellow]")

    # Parse loop bounds from annotations if requested (legacy mode)
    loop_bounds: list[int] = []
    if auto_bounds and not per_loop_bounds:
        loop_bounds = get_all_loop_bounds(ir_path=result.ir_path, c_path=c_file)
        if loop_bounds:
            console.print(f"[green]Found loop bounds:[/green] {loop_bounds}")
        else:
            console.print("[yellow]No loop bound annotations found[/yellow]")

    # Step 2: Generate DOT files
    with console.status("[bold green]Generating CFG..."):
        dot_files = generate_dot_files(result.ir_path, output_dir)

    if not dot_files:
        console.print("[yellow]No functions found in the C file.[/yellow]")
        raise SystemExit(0)

    console.print(f"[green]Found {len(dot_files)} function(s)[/green]")

    # Determine which bounds to analyze
    if auto_bounds and loop_bounds:
        bounds_to_analyze = loop_bounds
    else:
        bounds_to_analyze = [max_loop_iter]

    # Step 3: Analyze each function
    table = Table(title=f"CFG Analysis: {c_file.name}")
    table.add_column("Function", style="cyan")
    table.add_column("Basic Blocks", justify="right")
    table.add_column("Edges", justify="right")
    table.add_column("Has Loops", justify="center")

    # Add a column for each bound value
    if per_loop_bounds:
        table.add_column(f"Paths (per-loop)", justify="right")
    else:
        for bound in bounds_to_analyze:
            table.add_column(f"Paths (n={bound})", justify="right")

    for dot_file in sorted(dot_files):
        func_name = extract_function_name(dot_file)

        try:
            graph = parse_dot_file(dot_file)
        except Exception as e:
            console.print(f"[yellow]Warning: Could not parse {dot_file}: {e}[/yellow]")
            continue

        cfg = FunctionCFG(func_name, graph)
        stats = cfg.get_stats()

        # Count paths for each bound
        path_counts = []

        if per_loop_bounds and debug_info:
            # Extract node-to-block mapping from DOT labels
            node_to_block = extract_node_to_block_mapping(graph)

            # Use per-loop bounds from annotations
            loop_mapping = map_loops_to_bounds(
                cfg=graph,
                entry=cfg.entry,
                debug_info=debug_info,
                node_to_block=node_to_block,
                default_bound=max_loop_iter,
                function_name=func_name,
            )

            # Show per-loop bounds for this function
            if loop_mapping.loops:
                annotated = [l for l in loop_mapping.loops if l.bound is not None]
                if annotated:
                    console.print(f"  [dim]{func_name}: {len(annotated)} annotated loop(s)[/dim]")
                    for loop in annotated:
                        console.print(f"    [dim]Loop at line {loop.header_line}: bound={loop.bound}[/dim]")

            path_result = count_bounded_loop_paths_per_loop(
                cfg=graph,
                entry=cfg.entry,
                exits=cfg.exits,
                loop_mapping=loop_mapping,
            )
            path_counts.append(str(path_result.count))
        else:
            # Use global bound(s)
            for bound in bounds_to_analyze:
                path_result = count_paths(
                    graph,
                    cfg.entry,
                    cfg.exits,
                    strategy=strategy,
                    max_iterations=bound,
                )
                path_counts.append(str(path_result.count))

        table.add_row(
            func_name,
            str(stats.num_basic_blocks),
            str(stats.num_edges),
            "[yellow]Yes[/yellow]" if stats.has_loops else "No",
            *path_counts,
        )

        # Generate visualization
        if visualize:
            img_path = output_dir / f"{func_name}.{format}"
            try:
                visualize_cfg(graph, func_name, img_path, format)
                console.print(f"  [dim]Generated:[/dim] {img_path}")
            except Exception as e:
                console.print(f"  [yellow]Warning: Could not generate image: {e}[/yellow]")

    console.print()
    console.print(table)


@main.command()
def check():
    """Check if LLVM tools are available."""
    try:
        tools = find_llvm_tools()
        console.print(f"[green]clang:[/green] {tools.clang}")
        console.print(f"[green]opt:[/green] {tools.opt}")
        console.print("\n[green]LLVM tools are available![/green]")
    except RuntimeError as e:
        console.print(f"[red]{e}[/red]")
        raise SystemExit(1)


if __name__ == "__main__":
    main()

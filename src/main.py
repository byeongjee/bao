"""CLI entry point for checkpoint insertion optimization."""

import argparse
import sys
from pathlib import Path

from .ir_parser import CFG, build_cfg, compile_c_to_ir, parse_ir_module
from .optimizer import CheckpointOptimizer, InfeasibleBlockError


def main() -> int:
    """Main entry point for the checkpoint-insert CLI."""
    parser = argparse.ArgumentParser(
        prog="checkpoint-insert",
        description="MILP-based checkpoint insertion for intermittent computing",
    )
    parser.add_argument(
        "input",
        type=str,
        help="C source file (.c) or LLVM IR file (.ll)",
    )
    parser.add_argument(
        "--capacity", "-c",
        type=float,
        required=True,
        help="Energy capacity between checkpoints",
    )
    parser.add_argument(
        "--function", "-f",
        type=str,
        default="main",
        help="Function to analyze (default: main)",
    )
    parser.add_argument(
        "--output", "-o",
        type=str,
        default=None,
        help="Output file (default: stdout)",
    )
    parser.add_argument(
        "--verbose", "-v",
        action="store_true",
        help="Enable verbose output",
    )
    parser.add_argument(
        "--show-cfg",
        action="store_true",
        help="Print CFG information before optimization",
    )

    args = parser.parse_args()

    input_path = Path(args.input)

    if not input_path.exists():
        print(f"Error: Input file '{args.input}' not found", file=sys.stderr)
        return 1

    try:
        # Step 1: Get LLVM IR
        if args.verbose:
            print(f"Processing: {input_path}", file=sys.stderr)

        if input_path.suffix == ".c":
            if args.verbose:
                print("Compiling C to LLVM IR...", file=sys.stderr)
            ir_text = compile_c_to_ir(str(input_path))
        elif input_path.suffix == ".ll":
            if args.verbose:
                print("Reading LLVM IR file...", file=sys.stderr)
            ir_text = input_path.read_text()
        else:
            print(
                f"Error: Unsupported file type '{input_path.suffix}'. "
                "Use .c or .ll files.",
                file=sys.stderr,
            )
            return 1

        # Step 2: Parse IR and build CFG
        if args.verbose:
            print(f"Parsing IR and building CFG for function '{args.function}'...",
                  file=sys.stderr)

        module = parse_ir_module(ir_text)
        cfg = build_cfg(module, args.function)

        if args.show_cfg:
            _print_cfg_info(cfg)

        # Step 3: Optimize checkpoints
        if args.verbose:
            print(f"Optimizing with capacity={args.capacity}...", file=sys.stderr)

        optimizer = CheckpointOptimizer(cfg, args.capacity)
        optimizer.build_model()
        optimizer.solve(verbose=args.verbose)

        checkpoints = optimizer.get_checkpoints()

        # Step 4: Output results
        output_lines = []
        for block_name in checkpoints:
            output_lines.append(block_name)

        output_text = "\n".join(output_lines)
        if output_lines:
            output_text += "\n"

        if args.output:
            Path(args.output).write_text(output_text)
            if args.verbose:
                print(f"Results written to {args.output}", file=sys.stderr)
        else:
            print(output_text, end="")

        if args.verbose:
            print(f"\nTotal checkpoints: {len(checkpoints)}", file=sys.stderr)
            print(f"Objective value: {optimizer.get_objective_value():.2f}",
                  file=sys.stderr)

        return 0

    except InfeasibleBlockError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 2
    except ValueError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    except RuntimeError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1


def _print_cfg_info(cfg: CFG) -> None:
    """Print CFG information to stderr."""
    print("\n=== CFG Information ===", file=sys.stderr)
    print(f"Entry block: {cfg.entry_block}", file=sys.stderr)
    print(f"Number of blocks: {len(cfg.blocks())}", file=sys.stderr)
    print(f"Number of edges: {len(cfg.edges())}", file=sys.stderr)

    print("\nBlocks:", file=sys.stderr)
    for block_name in cfg.blocks():
        info = cfg.get_block_info(block_name)
        print(f"  {block_name}:", file=sys.stderr)
        print(f"    energy_cost: {info.energy_cost}", file=sys.stderr)
        print(f"    freq: {info.freq}", file=sys.stderr)
        print(f"    loop_depth: {info.loop_depth}", file=sys.stderr)

    print("\nEdges:", file=sys.stderr)
    for src, dst in cfg.edges():
        print(f"  {src} -> {dst}", file=sys.stderr)
    print("", file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main())

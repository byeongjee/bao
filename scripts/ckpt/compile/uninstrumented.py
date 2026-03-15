"""Uninstrumented compilation pipeline — baseline without checkpoint insertion.

Uses the same clang → opt → llc → gcc path as instrumented pipelines
for fair execution time comparison, but skips all checkpoint passes.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

from ..env import ProjectEnv
from ..toolchain import Toolchain
from .common import (
    compile_to_ir,
    compile_to_object,
    link_algorithm,
    optimize_ir,
)


@dataclass
class UninstrumentedCompileOptions:
    """Options for the uninstrumented compilation pipeline."""

    input_c: Path
    output: Path
    halt_mode: str
    debug_counters: bool
    cpu_freq: int
    opt_level: int
    clang_opt_level: int
    link: bool
    extra_includes: list[str] = field(default_factory=list)


@dataclass
class UninstrumentedCompileResult:
    """Result of an uninstrumented compilation."""

    object_file: Path
    assembly_file: Path
    elf_file: Path | None


def compile_uninstrumented(
    tc: Toolchain,
    env: ProjectEnv,
    opts: UninstrumentedCompileOptions,
) -> UninstrumentedCompileResult:
    """Compile without checkpoint insertion.

    Pipeline:
      compile_to_ir(-O0) → optimize_ir(-O2) → compile_to_object → link

    No checkpoint pass, no energy estimation, no profiling.
    """
    link = opts.link
    if opts.halt_mode in ("bor", "lpm4"):
        link = True
    if opts.debug_counters:
        link = True

    opts.output.parent.mkdir(parents=True, exist_ok=True)

    extra_includes = list(opts.extra_includes)
    extra_includes.append(str(env.project_dir / "passes" / "runtime"))

    # Phase 1: C → LLVM IR at -O0
    input_ll = opts.output.with_suffix(".input.ll")
    compile_to_ir(
        tc, env, opts.input_c, input_ll,
        clang_opt_level=0,
        debug=False,
        debug_counters=opts.debug_counters,
        extra_includes=extra_includes,
        extra_defines=[f"F_CPU={opts.cpu_freq}"],
    )

    # Phase 2: Optimize IR
    optimized_ll = input_ll
    if opts.clang_opt_level != 0:
        optimized_ll = opts.output.with_suffix(".optimized.ll")
        optimize_ir(tc, input_ll, optimized_ll, opt_level=opts.clang_opt_level)

    # Phase 3: Compile to MSP430 object
    out_s = opts.output.with_suffix(".s")
    out_o = opts.output.with_suffix(".o")
    compile_to_object(tc, env, optimized_ll, out_s, out_o, opt_level=opts.opt_level)

    # Phase 4: Link
    elf_file: Path | None = None
    if link:
        elf_file = _link_uninstrumented(tc, env, opts)

    return UninstrumentedCompileResult(
        object_file=out_o,
        assembly_file=out_s,
        elf_file=elf_file,
    )


def _link_uninstrumented(
    tc: Toolchain,
    env: ProjectEnv,
    opts: UninstrumentedCompileOptions,
) -> Path:
    """Link the uninstrumented output with boot.S + runtime.c."""
    boot_defines: list[str] = [f"F_CPU={opts.cpu_freq}"]

    return link_algorithm(
        tc, env,
        main_object=opts.output.with_suffix(".o"),
        output_elf=opts.output.with_suffix(".elf"),
        boot_source=env.uninstrumented_boot,
        runtime_source=env.uninstrumented_runtime,
        linker_script=env.milp_linker,
        boot_defines=boot_defines,
        debug_counters=opts.debug_counters,
        debug_counters_source=env.uninstrumented_debug_counters,
        cpu_freq=opts.cpu_freq,
    )

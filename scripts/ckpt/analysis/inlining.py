"""Code-size cost of forcing every function inline (paper §6.1).

Each benchmark marks its functions FORCE_INLINE (``always_inline``). This
builds every benchmark as is and with FORCE_INLINE reduced to plain
``static inline``, which leaves inlining to LLVM's O3 heuristics, and
compares the ``.text`` sizes of the uninstrumented object and ELF. The
functions O3 keeps out of line are those defined in its object but not in
the always_inline one.
"""

from __future__ import annotations

import csv
from pathlib import Path

from ..bench.config import discover_benchmarks
from ..compile.uninstrumented import (
    UninstrumentedCompileOptions,
    compile_uninstrumented,
)
from ..env import ProjectEnv
from ..errors import ConfigError
from ..runner import run
from ..toolchain import Toolchain

_FORCE_INLINE = "#define FORCE_INLINE static inline __attribute__((always_inline))"
_O3_INLINE = "#define FORCE_INLINE static inline"

CSV_HEADER = [
    "benchmark",
    "text_always_inline",
    "text_o3_inline",
    "delta_bytes",
    "delta_percent",
    "elf_text_always_inline",
    "elf_text_o3_inline",
    "o3_out_of_line_functions",
]


def _text_size(tc: Toolchain, path: Path) -> int:
    out = run([tc.size, "-A", str(path)], step_name="msp430-elf-size").stdout
    for line in out.splitlines():
        fields = line.split()
        if fields and fields[0] == ".text":
            return int(fields[1])
    raise ConfigError(f"No .text section in {path}")


def _functions(tc: Toolchain, path: Path) -> set[str]:
    out = run([tc.nm, str(path)], step_name="msp430-elf-nm").stdout
    return {
        fields[2]
        for fields in (line.split() for line in out.splitlines())
        if len(fields) == 3 and fields[1] in ("t", "T")
    }


def _compile(
    tc: Toolchain, env: ProjectEnv, source: Path, output: Path
) -> tuple[Path, Path]:
    result = compile_uninstrumented(
        tc,
        env,
        UninstrumentedCompileOptions(
            input_c=source,
            output=output,
            device_debug=False,
            cpu_freq=16_000_000,
            opt_level=3,
            clang_opt_level=3,
            link=True,
            # The O3 variant lives outside benchmarks/intermittent/ but
            # includes headers from there.
            extra_includes=[str(env.project_dir / "benchmarks" / "intermittent")],
        ),
    )
    if result.elf_file is None:
        raise ConfigError(f"No ELF produced for {source}")
    return result.object_file, result.elf_file


def measure_inlining(
    env: ProjectEnv, tc: Toolchain, *, result_dir: Path, workdir: Path
) -> None:
    """Write inlining.csv and numbers.txt into *result_dir*."""
    rows = []
    for source in discover_benchmarks(env, None):
        name = source.stem
        text = source.read_text()
        if _FORCE_INLINE not in text:
            raise ConfigError(f"{source} does not define FORCE_INLINE as expected")
        o3_source = workdir / "src" / f"{name}.c"
        o3_source.parent.mkdir(parents=True, exist_ok=True)
        o3_source.write_text(text.replace(_FORCE_INLINE, _O3_INLINE))

        forced_o, forced_elf = _compile(tc, env, source, workdir / "always" / name)
        o3_o, o3_elf = _compile(tc, env, o3_source, workdir / "o3" / name)

        forced, o3 = _text_size(tc, forced_o), _text_size(tc, o3_o)
        rows.append(
            {
                "benchmark": name,
                "text_always_inline": forced,
                "text_o3_inline": o3,
                "delta_bytes": forced - o3,
                "delta_percent": f"{100 * (forced - o3) / o3:.1f}",
                "elf_text_always_inline": _text_size(tc, forced_elf),
                "elf_text_o3_inline": _text_size(tc, o3_elf),
                "o3_out_of_line_functions": ";".join(
                    sorted(_functions(tc, o3_o) - _functions(tc, forced_o))
                ),
            }
        )

    result_dir.mkdir(parents=True, exist_ok=True)
    with open(result_dir / "inlining.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=CSV_HEADER, lineterminator="\n")
        w.writeheader()
        w.writerows(rows)

    grown = [r for r in rows if r["o3_out_of_line_functions"]]
    percents = [float(r["delta_percent"]) for r in grown]
    inlined = len(rows) - len(grown)
    largest_kb = max(int(r["delta_bytes"]) for r in grown) / 1000
    growth = f"{min(percents):.0f}--{max(percents):.0f}%"
    lines = [
        f"§6.1: benchmarks whose functions O3 already inlines = {inlined}/{len(rows)}",
        f"§6.1: code-size growth of always_inline on the rest = {growth}",
        f"§6.1: largest code-size growth = {largest_kb:.1f} kB",
    ]
    (result_dir / "numbers.txt").write_text("\n".join(lines) + "\n")

"""Shared compilation steps used by all pipelines.

Replaces compile_to_ir(), run_assembly_energy(), collect_bb_freq(), and
other shared helpers from lib/common.sh and the individual compile_*.sh
scripts.
"""

from __future__ import annotations

import json
import time
from pathlib import Path

from ..env import ProjectEnv
from ..runner import CompilationError, StepResult, run
from ..toolchain import Toolchain


# ---------------------------------------------------------------------------
# C -> LLVM IR
# ---------------------------------------------------------------------------

def compile_to_ir(
    tc: Toolchain,
    env: ProjectEnv,
    input_c: Path,
    output_ll: Path,
    *,
    clang_opt_level: int = 2,
    debug: bool,
    debug_counters: bool,
    extra_includes: list[str] | None = None,
    extra_defines: list[str] | None = None,
) -> StepResult:
    """Compile a C source file to LLVM IR targeting msp430-elf."""
    cmd: list[str] = [
        tc.clang,
        "--target=msp430-elf",
        "-S", "-emit-llvm",
        f"-O{clang_opt_level}",
        "-D__MSP430FR5994__",
        f"-I{env.project_dir / 'passes' / 'include'}",
        "-isystem", str(env.msp430gcc_support_path / "include"),
        "-isystem", str(env.msp430gcc_support_path / "msp430-elf" / "include"),
    ]

    if clang_opt_level == 0:
        cmd += ["-Xclang", "-disable-O0-optnone"]

    for inc in extra_includes or []:
        cmd.append(f"-I{inc}")
    for defn in extra_defines or []:
        cmd.append(f"-D{defn}")

    if debug:
        cmd.append("-DDEBUG")
    if debug_counters:
        cmd.append("-DDEBUG_COUNTERS")

    cmd += [str(input_c), "-o", str(output_ll)]

    return run(cmd, step_name="compile_to_ir")


# ---------------------------------------------------------------------------
# Trip-count annotation
# ---------------------------------------------------------------------------

def annotate_tripcounts(
    tc: Toolchain,
    env: ProjectEnv,
    input_ll: Path,
    output_ll: Path,
) -> StepResult:
    """Run the tripcount-annotation pass on LLVM IR."""
    return run(
        [
            tc.opt,
            f"-load-pass-plugin={env.pass_lib}",
            "-passes=tripcount-annotation",
            "-S", str(input_ll),
            "-o", str(output_ll),
        ],
        step_name="tripcount-annotation",
    )


# ---------------------------------------------------------------------------
# IR optimization
# ---------------------------------------------------------------------------

def optimize_ir(
    tc: Toolchain,
    input_ll: Path,
    output_ll: Path,
    *,
    opt_level: int = 2,
) -> StepResult:
    """Run the LLVM default optimization pipeline with vectorization disabled."""
    return run(
        [
            tc.opt,
            f"-passes=default<O{opt_level}>",
            "-vectorize-loops=false",
            "-vectorize-slp=false",
            "-S", str(input_ll),
            "-o", str(output_ll),
        ],
        step_name=f"optimize-ir-O{opt_level}",
    )


# ---------------------------------------------------------------------------
# Assembly-based energy estimation
# ---------------------------------------------------------------------------


def run_assembly_energy(
    tc: Toolchain,
    env: ProjectEnv,
    input_ll: Path,
    prefix: Path,
    params_config: Path,
) -> tuple[Path, str]:
    """Run the assembly-based BB energy estimation pipeline.

    Steps:
      1. assign-bb-debuginfo pass  ->  <prefix>.bbinfo.ll + <prefix>.bb_mapping.json
      2. llc -march=msp430 to obj  ->  <prefix>.energy.o
      3. bb-energy-analyzer         ->  <prefix>.bb_energy.json

    Returns (bb_energy_path, analyzer_stderr).
    """
    bbinfo_ll = Path(f"{prefix}.bbinfo.ll")
    bb_mapping = Path(f"{prefix}.bb_mapping.json")
    energy_obj = Path(f"{prefix}.energy.o")
    bb_energy = Path(f"{prefix}.bb_energy.json")

    # Step 1: assign-bb-debuginfo
    run(
        [
            tc.opt,
            f"-load-pass-plugin={env.bb_debuginfo_lib}",
            "-passes=assign-bb-debuginfo",
            f"-bb-mapping={bb_mapping}",
            "-S", str(input_ll),
            "-o", str(bbinfo_ll),
        ],
        step_name="assign-bb-debuginfo",
    )

    # Step 2: llc to object
    run(
        [
            tc.llc,
            "-march=msp430",
            "-filetype=obj",
            str(bbinfo_ll),
            "-o", str(energy_obj),
        ],
        step_name="llc-energy-obj",
    )

    # Step 3: bb-energy-analyzer
    result = run(
        [
            str(env.bb_analyzer),
            "--energy-params", str(params_config),
            "--bb-mapping", str(bb_mapping),
            str(energy_obj),
        ],
        step_name="bb-energy-analyzer",
    )

    bb_energy.write_text(result.stdout)
    return bb_energy, result.stderr


# ---------------------------------------------------------------------------
# BB frequency profiling
# ---------------------------------------------------------------------------

_DEBUG_STUBS_C = """\
void debug_init(void) {}
void debug_exit(int result) { (void)result; }
"""


def collect_bb_freq(
    tc: Toolchain,
    env: ProjectEnv,
    inst_ll: Path,
    workdir: Path,
) -> Path:
    """Compile and run a native BB frequency profiling binary.

    Strips the MSP430 target triple from the instrumented IR so that clang
    can build a host-native executable, then runs it to produce bb_freq.json.

    Returns the path to the generated bb_freq.json.
    """
    native_ll = workdir / "freq_inst_native.ll"
    stubs_c = workdir / "debug_stubs.c"
    freq_bin = workdir / "freq_run"
    bb_freq_json = workdir / "bb_freq.json"

    # Strip MSP430 target triple and datalayout
    ir_text = inst_ll.read_text()
    lines: list[str] = []
    for line in ir_text.splitlines(keepends=True):
        if line.startswith("target triple = "):
            lines.append("\n")
        elif line.startswith("target datalayout = "):
            lines.append("\n")
        else:
            lines.append(line)
    native_ll.write_text("".join(lines))

    # Write host stubs
    stubs_c.write_text(_DEBUG_STUBS_C)

    # Compile native binary
    compile_cmd: list[str] = [
        tc.clang, "-O0",
        *env.sysroot_flags,
        str(native_ll),
        str(env.bb_freq_runtime),
        str(stubs_c),
        "-o", str(freq_bin),
    ]
    run(compile_cmd, step_name="bb-freq-compile")

    # Run the profiling binary (cwd must be workdir so it writes bb_freq.json there)
    run(
        [str(freq_bin)],
        check=False,
        step_name="bb-freq-run",
        cwd=str(workdir),
    )

    if not bb_freq_json.exists():
        raise CompilationError(
            "bb-freq-collect",
            StepResult(1, "", "bb_freq.json was not produced", 0),
        )

    return bb_freq_json


# ---------------------------------------------------------------------------
# Compile IR to MSP430 object
# ---------------------------------------------------------------------------

def compile_to_object(
    tc: Toolchain,
    env: ProjectEnv,
    input_ll: Path,
    output_s: Path,
    output_o: Path,
    *,
    opt_level: int = 2,
) -> StepResult:
    """Lower LLVM IR to MSP430 assembly, then assemble to an object file.

    Steps:
      1. llc -march=msp430  ->  .s
      2. gcc -mmcu=DEVICE    ->  .o
    """
    run(
        [
            tc.llc,
            "-march=msp430",
            f"-O{opt_level}",
            str(input_ll),
            "-o", str(output_s),
        ],
        step_name="llc-to-asm",
    )

    return run(
        [
            tc.gcc,
            f"-mmcu={env.device}",
            "-msmall",
            f"-I{env.msp430gcc_support_path / 'include'}",
            "-c", str(output_s),
            "-o", str(output_o),
        ],
        step_name="gcc-assemble",
    )


# ---------------------------------------------------------------------------
# Link
# ---------------------------------------------------------------------------

def assemble_and_link(
    tc: Toolchain,
    env: ProjectEnv,
    objects: list[Path],
    output_elf: Path,
    *,
    linker_script: Path,
    extra_flags: list[str] | None = None,
) -> StepResult:
    """Link object files into an MSP430 ELF binary."""
    cmd: list[str] = [
        tc.gcc,
        f"-mmcu={env.device}",
        "-msmall",
        f"-L{env.msp430gcc_support_path / 'include'}",
        "-T", str(linker_script),
        "-Wl,--nmagic",
    ]
    cmd += [str(o) for o in objects]
    cmd += extra_flags or []
    cmd += ["-o", str(output_elf)]

    return run(cmd, step_name="link")


# ---------------------------------------------------------------------------
# Runtime compilation helpers
# ---------------------------------------------------------------------------

def compile_runtime_c(
    tc: Toolchain,
    env: ProjectEnv,
    source: Path,
    output_o: Path,
    *,
    extra_defines: list[str] | None = None,
) -> StepResult:
    """Compile a runtime C source file for MSP430."""
    cmd: list[str] = [
        tc.gcc,
        f"-mmcu={env.device}",
        "-msmall",
        "-O2",
        f"-I{env.msp430gcc_support_path / 'include'}",
        f"-I{env.project_dir / 'passes' / 'runtime'}",
    ]
    for defn in extra_defines or []:
        cmd.append(f"-D{defn}")
    cmd += ["-c", str(source), "-o", str(output_o)]

    return run(cmd, step_name=f"compile-runtime-{source.stem}")


def assemble_boot(
    tc: Toolchain,
    env: ProjectEnv,
    source: Path,
    output_o: Path,
    *,
    extra_defines: list[str] | None = None,
) -> StepResult:
    """Assemble a boot .S file for MSP430."""
    cmd: list[str] = [
        tc.gcc,
        f"-mmcu={env.device}",
        "-msmall",
    ]
    for defn in extra_defines or []:
        cmd.append(f"-D{defn}")
    cmd += ["-c", str(source), "-o", str(output_o)]

    return run(cmd, step_name=f"assemble-boot-{source.stem}")


# ---------------------------------------------------------------------------
# Temporary energy config generation
# ---------------------------------------------------------------------------

def write_assembly_energy_config(path: Path, energy_data_path: Path) -> Path:
    """Write a temporary energy config JSON pointing to assembly energy data."""
    config = {
        "estimator_type": "assembly",
        "energy_parameters": {
            "energy_data_path": str(energy_data_path),
        },
    }
    path.write_text(json.dumps(config))
    return path


# ---------------------------------------------------------------------------
# Timing helper
# ---------------------------------------------------------------------------

def now_ms() -> int:
    """Return the current time in milliseconds (monotonic clock)."""
    return int(time.monotonic() * 1000)


# ---------------------------------------------------------------------------
# Shared link step
# ---------------------------------------------------------------------------

def link_algorithm(
    tc: Toolchain,
    env: ProjectEnv,
    *,
    main_object: Path,
    output_elf: Path,
    boot_source: Path,
    runtime_source: Path,
    linker_script: Path,
    boot_defines: list[str] | None = None,
    debug_counters: bool = False,
    debug_counters_source: Path | None = None,
) -> Path:
    """Assemble boot + runtime and link with the main object into an ELF.

    This is the shared link step used by all three algorithm pipelines.
    """
    stem = output_elf.with_suffix("")

    boot_o = stem.with_suffix(".boot.o")
    assemble_boot(tc, env, boot_source, boot_o, extra_defines=boot_defines or [])

    runtime_o = stem.with_suffix(".runtime.o")
    compile_runtime_c(tc, env, runtime_source, runtime_o)

    link_objs = [main_object, boot_o, runtime_o]

    if debug_counters and debug_counters_source is not None:
        debug_o = stem.with_suffix(".debug_counters.o")
        compile_runtime_c(
            tc, env, debug_counters_source, debug_o,
            extra_defines=["DEBUG_COUNTERS"],
        )
        link_objs.append(debug_o)

    assemble_and_link(tc, env, link_objs, output_elf, linker_script=linker_script)
    return output_elf

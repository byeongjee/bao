"""Shared compilation steps used by all pipelines.

Replaces compile_to_ir(), run_assembly_energy(), collect_bb_freq(), and
other shared helpers from lib/common.sh and the individual compile_*.sh
scripts.
"""

from __future__ import annotations

import functools
import json
import re
import shutil
import time
from pathlib import Path

from ..env import ProjectEnv
from ..errors import CompilationError, ToolError
from ..runner import StepResult, run
from ..toolchain import Toolchain

MATH_LINK_FLAGS = ["-lm"]


def raises_compilation_error(fn):
    """Convert ToolError from subprocess steps into CompilationError.

    ``runner.run`` raises the generic ToolError; compile pipelines report
    failures as CompilationError (exit code 4, caught by bench/verify
    loops). Applied to each ``compile_*`` entry point.
    """

    @functools.wraps(fn)
    def wrapper(*args, **kwargs):
        try:
            return fn(*args, **kwargs)
        except CompilationError:
            raise
        except ToolError as exc:
            raise CompilationError(exc.step, exc.result) from exc

    return wrapper


# ---------------------------------------------------------------------------
# C -> LLVM IR
# ---------------------------------------------------------------------------


def compile_to_ir(
    tc: Toolchain,
    env: ProjectEnv,
    input_c: Path,
    output_ll: Path,
    *,
    debug: bool,
    device_debug: bool,
    extra_includes: list[str] | None = None,
    extra_defines: list[str] | None = None,
) -> StepResult:
    """Emit raw frontend LLVM IR for a C source file (msp430-elf).

    ``-O3 -Xclang -disable-llvm-passes`` is clang's IRGen output with no pass
    run: untransformed code whose loop structure still matches the source,
    carrying the optimizer metadata (TBAA, lifetimes) and exactly the function
    attributes written in the source (no clang-added blanket ``noinline``).
    The -O3 here selects what IRGen emits, not an optimization level — all
    optimization happens later in optimize_ir, whose level is the only
    optimization knob.
    """
    cmd: list[str] = [
        tc.clang,
        "--target=msp430-elf",
        "-S",
        "-emit-llvm",
        "-O3",
        "-Xclang",
        "-disable-llvm-passes",
        "-D__MSP430FR5994__",
        f"-I{env.project_dir / 'passes' / 'include'}",
        "-isystem",
        str(env.msp430gcc_support_path / "include"),
        "-isystem",
        str(env.msp430gcc_support_path / "msp430-elf" / "include"),
    ]

    for inc in extra_includes or []:
        cmd.append(f"-I{inc}")
    for defn in extra_defines or []:
        cmd.append(f"-D{defn}")

    if debug:
        cmd.append("-DDEBUG")
    if device_debug:
        cmd.append("-DDEVICE_DEBUG")

    cmd += [str(input_c), "-o", str(output_ll)]

    return run(cmd, step_name="compile_to_ir")


def compile_annotated_ir(
    tc: Toolchain,
    env: ProjectEnv,
    *,
    input_c: Path,
    tmp: Path,
    debug: bool,
    device_debug: bool,
    cpu_freq: int,
    extra_includes: list[str],
    extra_defines: list[str],
) -> Path:
    """Shared phase 1 of the instrumented pipelines: C -> raw IR -> tripcounts.

    No LLVM pass has run on the returned IR, so the ``__loop_tripcount``
    markers still sit inside their source loops for exact trip-count
    annotation. Returns the annotated IR path (tmp/tripcount.ll).
    """
    input_ll = tmp / "input.ll"
    includes = list(extra_includes)
    includes.append(str(env.project_dir / "passes" / "runtime"))

    compile_to_ir(
        tc,
        env,
        input_c,
        input_ll,
        debug=debug,
        device_debug=device_debug,
        extra_includes=includes,
        extra_defines=[f"F_CPU={cpu_freq}", *extra_defines],
    )

    tripcount_ll = tmp / "tripcount.ll"
    annotate_tripcounts(tc, env, input_ll, tripcount_ll)
    return tripcount_ll


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
            "-S",
            str(input_ll),
            "-o",
            str(output_ll),
        ],
        step_name="tripcount-annotation",
    )


def isolate_calls(
    tc: Toolchain,
    env: ProjectEnv,
    input_ll: Path,
    output_ll: Path,
) -> StepResult:
    """Run the SCHEMATIC call-isolation pass on LLVM IR.

    Splits every isolatable call into pre -> call_entry -> call_exit -> post and
    marks the split blocks so the (inter-procedural) SCHEMATIC pass can fold each
    callee's summary onto its call sites. Replaces the old inline_functions step:
    instead of flattening the module into one function, calls are kept and solved
    bottom-up. Recursion is rejected here with a fatal error.
    """
    return run(
        [
            tc.opt,
            f"-load-pass-plugin={env.pass_lib}",
            "-passes=schematic-isolate",
            "-S",
            str(input_ll),
            "-o",
            str(output_ll),
        ],
        step_name="schematic-isolate",
    )


# ---------------------------------------------------------------------------
# IR optimization
# ---------------------------------------------------------------------------


def optimize_ir(
    tc: Toolchain,
    input_ll: Path,
    output_ll: Path,
    *,
    opt_level: int,
) -> StepResult:
    """Run the LLVM default optimization pipeline with vectorization disabled."""
    return optimize_ir_with_options(
        tc,
        input_ll,
        output_ll,
        opt_level=opt_level,
        disable_loop_unrolling=False,
    )


def optimize_ir_with_options(
    tc: Toolchain,
    input_ll: Path,
    output_ll: Path,
    *,
    opt_level: int,
    disable_loop_unrolling: bool,
) -> StepResult:
    """Run the LLVM default optimization pipeline with explicit pass controls.

    A trailing ``scalarizer<load-store>`` expands any vector load/store the O3
    pipeline forms (e.g. SROA promoting a small struct copy to ``<3 x i16>``)
    back into scalars.  The MSP430 backend has no vector support and asserts in
    type legalization on such vectors, so this keeps the optimized IR lowerable
    by ``llc -march=msp430``.  It is a no-op on scalar-only IR, and runs at the
    single optimization point shared by energy estimation and device codegen, so
    both consume identical IR.
    """
    cmd: list[str] = [
        tc.opt,
        f"-passes=default<O{opt_level}>,scalarizer<load-store>",
        "-vectorize-loops=false",
        "-vectorize-slp=false",
    ]
    if disable_loop_unrolling:
        cmd.append("-disable-loop-unrolling")
    cmd += ["-S", str(input_ll), "-o", str(output_ll)]

    return run(
        cmd,
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
    pass_log_level: str,
    *,
    opt_level: int,
) -> tuple[Path, str]:
    """Run the assembly-based BB energy estimation pipeline.

    Steps:
      1. assign-bb-debuginfo pass  ->  <prefix>.bbinfo.ll + <prefix>.bb_mapping.json
      2. llc -march=msp430 to obj  ->  <prefix>.energy.o
      3. bb-energy-analyzer         ->  <prefix>.bb_energy.json

    *opt_level* is the ``llc`` optimization level; pass the same value used for
    device codegen (``compile_to_object``) so the assembly measured for energy
    matches the assembly that actually runs.

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
            "-S",
            str(input_ll),
            "-o",
            str(bbinfo_ll),
        ],
        step_name="assign-bb-debuginfo",
    )

    # Step 2: llc to object (same -O as device codegen for a faithful estimate)
    run(
        [
            tc.llc,
            "-march=msp430",
            "-filetype=obj",
            f"-O{opt_level}",
            str(bbinfo_ll),
            "-o",
            str(energy_obj),
        ],
        step_name="llc-energy-obj",
    )

    # Step 3: bb-energy-analyzer
    result = run(
        [
            str(env.bb_analyzer),
            "--energy-params",
            str(params_config),
            "--bb-mapping",
            str(bb_mapping),
            f"-ckpt-log-level={pass_log_level}",
            str(energy_obj),
        ],
        step_name="bb-energy-analyzer",
    )

    bb_energy.write_text(result.stdout)
    return bb_energy, result.stderr


# ---------------------------------------------------------------------------
# BB frequency profiling
# ---------------------------------------------------------------------------

_NATIVE_STUBS_C = """\
void debug_init(void) {}
void debug_exit(int result) { (void)result; }
void bench_halt(void) {}
/* GPIO and clock register stubs for native profiling.
   benchmark.h's timing_gpio_init() references MSP430 hardware registers
   that get baked into the LLVM IR when compiled with --target=msp430-elf. */
unsigned char P3DIR, P3OUT;
unsigned char CSCTL0_H;
unsigned int CSCTL1, CSCTL2, CSCTL3;
unsigned int PM5CTL0;
unsigned int FRCTL0;
#define FRCTLPW 0
#define NWAITS_1 0
"""


def strip_ir_for_native(input_ll: Path, output_ll: Path) -> None:
    """Strip MSP430 target info from LLVM IR for native host compilation.

    Removes the target triple, datalayout, ELF-only section attributes,
    and @llvm.compiler.used/@llvm.used so clang can compile natively on
    the host (e.g., Mach-O on macOS where ".fram" sections are invalid).
    """
    ir_text = input_ll.read_text()
    lines: list[str] = []
    for line in ir_text.splitlines(keepends=True):
        if (
            line.startswith(("target triple = ", "target datalayout = "))
            or "@llvm.compiler.used" in line
            or "@llvm.used" in line
        ):
            lines.append("\n")
        else:
            line = re.sub(r',?\s*section\s+"[^"]+"', "", line)
            lines.append(line)
    output_ll.write_text("".join(lines))


def canonicalize_ir_for_native_profiling(
    tc: Toolchain,
    input_ll: Path,
    output_ll: Path,
) -> StepResult:
    """Canonicalize IR before native profiling compilation.

    Native profiling recompiles MSP430-targeted IR for the host.  Some O0 traces
    keep aggregate copies and field accesses whose semantics depend on the MSP430
    datalayout.  A small scalarization pipeline rewrites those accesses while the
    original target datalayout is still present, which keeps later host execution
    semantically aligned with the profiled target code.
    """
    return run(
        [
            tc.opt,
            "-passes=sroa,instcombine,gvn",
            "-S",
            str(input_ll),
            "-o",
            str(output_ll),
        ],
        step_name="native-profile-canonicalize",
    )


def write_native_stubs(stubs_c: Path) -> None:
    """Write C stub file for MSP430 hardware registers used by benchmark.h."""
    stubs_c.write_text(_NATIVE_STUBS_C)


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

    strip_ir_for_native(inst_ll, native_ll)
    write_native_stubs(stubs_c)

    # Compile native binary
    compile_cmd: list[str] = [
        tc.clang,
        "-O0",
        *env.sysroot_flags,
        str(native_ll),
        str(env.bb_freq_runtime),
        str(stubs_c),
        *MATH_LINK_FLAGS,
        "-o",
        str(freq_bin),
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
    opt_level: int,
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
            "-o",
            str(output_s),
        ],
        step_name="llc-to-asm",
    )

    return run(
        [
            tc.gcc,
            f"-mmcu={env.device}",
            "-msmall",
            f"-I{env.msp430gcc_support_path / 'include'}",
            "-c",
            str(output_s),
            "-o",
            str(output_o),
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
        "-T",
        str(linker_script),
        "-Wl,--nmagic",
    ]
    cmd += [str(o) for o in objects]
    cmd += extra_flags or []
    cmd += MATH_LINK_FLAGS
    cmd += ["-o", str(output_elf)]

    return run(cmd, step_name="link")


def build_boot_defines(
    *,
    cpu_freq: int,
    halt_mode: str | None,
    device_debug: bool,
) -> list[str]:
    """Build the -D defines for assembling a checkpoint pipeline's boot.S."""
    defines = [f"F_CPU={cpu_freq}"]
    if halt_mode == "bor":
        defines.append("HALT_BOR")
    elif halt_mode == "lpm4":
        defines.append("HALT_LPM4")
    elif halt_mode == "swbor":
        defines.append("HALT_SWBOR")
    if device_debug:
        defines.append("DEVICE_DEBUG")
    return defines


# ---------------------------------------------------------------------------
# Runtime compilation helpers
# ---------------------------------------------------------------------------


def compile_runtime_c(
    tc: Toolchain,
    env: ProjectEnv,
    source: Path,
    output_o: Path,
    *,
    gcc_opt_level: int,
    extra_defines: list[str] | None = None,
) -> StepResult:
    """Compile a runtime C source file for MSP430."""
    cmd: list[str] = [
        tc.gcc,
        f"-mmcu={env.device}",
        "-msmall",
        f"-O{gcc_opt_level}",
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


def save_temps(tmp: Path, dest_dir: Path) -> None:
    """Copy all intermediate files from *tmp* to *dest_dir*."""
    dest_dir.mkdir(parents=True, exist_ok=True)
    for f in sorted(tmp.iterdir()):
        if f.is_file():
            shutil.copy2(f, dest_dir / f.name)


def copy_stats_json(tmp: Path, output: Path) -> Path | None:
    """Copy tmp/stats.json next to *output* as <output>.stats.json if present."""
    src = tmp / "stats.json"
    if not src.is_file():
        return None
    dst = output.with_suffix(".stats.json")
    shutil.copy2(src, dst)
    return dst


def finalize_checkpointed_object(
    tc: Toolchain,
    env: ProjectEnv,
    *,
    tmp: Path,
    output: Path,
    opt_level: int,
    link: bool,
    link_fn,
    pass_output: str,
    stats_json: Path | None,
    save_temps_dir: Path | None,
) -> Path | None:
    """Compile tmp/ckpt.ll to <output>.s/.o and optionally link via *link_fn*.

    Failures raise CompilationError carrying *pass_output* and *stats_json*
    so pass statistics survive post-pass errors. Temps are saved to
    *save_temps_dir* (when set) on both success and failure. Returns the
    ELF path when linked, else None.
    """
    elf_file: Path | None = None
    try:
        out_s = tmp / "ckpt.s"
        out_o = tmp / "ckpt.o"
        compile_to_object(tc, env, tmp / "ckpt.ll", out_s, out_o, opt_level=opt_level)

        if save_temps_dir is not None:
            save_temps(tmp, save_temps_dir)

        shutil.copy2(out_o, output.with_suffix(".o"))
        shutil.copy2(out_s, output.with_suffix(".s"))

        if link:
            elf_file = link_fn()
    except ToolError as exc:
        if save_temps_dir is not None:
            save_temps(tmp, save_temps_dir)
        err = CompilationError(exc.step, exc.result)
        err.pass_output = pass_output
        err.stats_json = stats_json
        raise err from exc
    return elf_file


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
    cpu_freq: int,
    gcc_opt_level: int,
    boot_defines: list[str] | None,
    device_debug: bool,
) -> Path:
    """Assemble boot + runtime and link with the main object into an ELF.

    This is the shared link step used by all three algorithm pipelines.
    When device_debug is True, the runtime is compiled with -DDEVICE_DEBUG
    which enables NVM result storage, debug counters, and UART output.
    debug_common.c is also compiled and linked to provide shared UART
    and debug infrastructure.
    """
    stem = output_elf.with_suffix("")

    boot_o = stem.with_suffix(".boot.o")
    assemble_boot(tc, env, boot_source, boot_o, extra_defines=boot_defines or [])

    runtime_defines: list[str] = [f"F_CPU={cpu_freq}"]
    if device_debug:
        runtime_defines.append("DEVICE_DEBUG")

    runtime_o = stem.with_suffix(".runtime.o")
    compile_runtime_c(
        tc,
        env,
        runtime_source,
        runtime_o,
        gcc_opt_level=gcc_opt_level,
        extra_defines=runtime_defines,
    )

    link_objs = [main_object, boot_o, runtime_o]

    if device_debug:
        debug_common_o = stem.with_suffix(".debug_common.o")
        compile_runtime_c(
            tc,
            env,
            env.debug_common_c,
            debug_common_o,
            gcc_opt_level=gcc_opt_level,
            extra_defines=runtime_defines,
        )
        link_objs.append(debug_common_o)

    assemble_and_link(tc, env, link_objs, output_elf, linker_script=linker_script)
    return output_elf

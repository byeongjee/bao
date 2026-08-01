"""MILP compilation pipeline — replaces compile_milp.sh.

Supports two estimator modes:
  - assembly (two-pass): pre/post strip-mining energy estimation via bb-energy-analyzer
  - ir (single-pass): IR-level energy estimation
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

from ..env import ProjectEnv
from ..errors import ToolError
from ..runner import run
from ..tempdir import compilation_workdir
from ..toolchain import Toolchain
from . import common
from .common import (
    collect_bb_freq,
    compile_annotated_ir,
    link_algorithm,
    now_ms,
    optimize_ir,
    run_assembly_energy,
    write_assembly_energy_config,
)


@dataclass
class MilpCompileOptions:
    """Options for the MILP compilation pipeline."""

    input_c: Path
    energy_config: Path
    milp_config: Path
    output: Path
    estimator_mode: str
    pass_log_level: str
    debug: bool
    link: bool
    halt_mode: str | None
    device_debug: bool
    cpu_freq: int
    opt_level: int
    clang_opt_level: int
    milp_gap: float
    milp_log_file: str
    coarse_allocation: bool
    save_temps: bool
    extra_includes: list[str] = field(default_factory=list)


@dataclass
class MilpCompileResult:
    """Result of a MILP compilation."""

    object_file: Path
    assembly_file: Path
    elf_file: Path | None
    pass_output: str
    profiling_time_ms: int
    stats_json: Path | None


@common.raises_compilation_error
def compile_milp(
    tc: Toolchain,
    env: ProjectEnv,
    opts: MilpCompileOptions,
) -> MilpCompileResult:
    """Run the full MILP checkpoint insertion pipeline.

    Assembly mode (two-pass):
      compile_to_ir(-O0) -> tripcount annotation -> optimize_ir ->
      pre-strip-mining assembly energy -> milp-preprocess ->
      post-strip-mining assembly energy -> bb-freq-collect-only + collect_bb_freq ->
      milp-solve-only -> compile to object

    IR mode (single-pass):
      compile_to_ir(-O0) -> tripcount annotation -> optimize_ir ->
      bb-freq-collect -> collect_bb_freq -> milp pass -> compile to object

    Both modes optionally link with milp_boot.S + milp_runtime.c.
    """
    # bor/lpm4/swbor halt modes and debug-counters imply linking
    link = opts.link
    if opts.halt_mode in ("bor", "lpm4", "swbor"):
        link = True
    if opts.device_debug:
        link = True

    opts.output.parent.mkdir(parents=True, exist_ok=True)

    with compilation_workdir(prefix="ckpt_milp_") as tmp:
        tripcount_ll = compile_annotated_ir(
            tc,
            env,
            input_c=opts.input_c,
            tmp=tmp,
            # -O0 keeps clang's blanket noinline on every function, so the
            # later optimize_ir never inlines — this pipeline depends on that
            # to keep functions separate.
            raw_frontend=False,
            debug=opts.debug,
            device_debug=opts.device_debug,
            cpu_freq=opts.cpu_freq,
            extra_includes=opts.extra_includes,
        )

        # Frontend optimization
        milp_input_ll = tripcount_ll
        if opts.clang_opt_level != 0:
            optimized_ll = tmp / "input_optimized.ll"
            optimize_ir(tc, tripcount_ll, optimized_ll, opt_level=opts.clang_opt_level)
            milp_input_ll = optimized_ll

        # Build extra flags for MILP passes
        milp_extra_flags: list[str] = []
        if opts.device_debug:
            milp_extra_flags.append("-add-debug-markers")
        milp_extra_flags.append(f"-ckpt-log-level={opts.pass_log_level}")
        milp_extra_flags.append(f"-milp-gap={opts.milp_gap}")
        if opts.milp_log_file:
            milp_extra_flags.append(f"-milp-log-file={opts.milp_log_file}")

        try:
            if opts.estimator_mode == "assembly":
                pass_output, profiling_ms, strip_mining_stats_json = _assembly_mode(
                    tc,
                    env,
                    opts,
                    tmp,
                    milp_input_ll,
                    milp_extra_flags,
                )
            else:
                pass_output, profiling_ms, strip_mining_stats_json = _ir_mode(
                    tc,
                    env,
                    opts,
                    tmp,
                    milp_input_ll,
                    milp_extra_flags,
                )
        except ToolError:
            if opts.save_temps:
                common.save_temps(tmp, opts.output.parent)
            raise

        _merge_strip_mining_stats(tmp / "stats.json", strip_mining_stats_json)

        stats_json = common.copy_stats_json(tmp, opts.output)

        elf_file = common.finalize_checkpointed_object(
            tc,
            env,
            tmp=tmp,
            output=opts.output,
            opt_level=opts.opt_level,
            link=link,
            link_fn=lambda: _link_milp(tc, env, opts),
            pass_output=pass_output,
            stats_json=stats_json,
            save_temps_dir=opts.output.parent if opts.save_temps else None,
        )

    return MilpCompileResult(
        object_file=opts.output.with_suffix(".o"),
        assembly_file=opts.output.with_suffix(".s"),
        elf_file=elf_file,
        pass_output=pass_output,
        profiling_time_ms=profiling_ms,
        stats_json=stats_json,
    )


# ---------------------------------------------------------------------------
# Assembly-mode two-pass pipeline
# ---------------------------------------------------------------------------


def _assembly_mode(
    tc: Toolchain,
    env: ProjectEnv,
    opts: MilpCompileOptions,
    tmp: Path,
    milp_input_ll: Path,
    milp_extra_flags: list[str],
) -> tuple[str, int, Path]:
    """Assembly-based two-pass energy estimation pipeline.

    Returns (pass_output, profiling_time_ms).
    """
    # Phase 2: Pre-strip-mining assembly energy
    pre_bb_energy, pre_stderr = run_assembly_energy(
        tc,
        env,
        milp_input_ll,
        tmp / "pre",
        opts.energy_config,
        opts.pass_log_level,
        opt_level=opts.opt_level,
    )

    pre_energy_config = write_assembly_energy_config(
        tmp / "pre_energy_config.json",
        pre_bb_energy,
    )
    strip_mining_stats_json = tmp / "strip_mining_stats.json"

    # Phase 3: Preprocessing (loop canonicalization + strip-mining)
    preprocessed_ll = tmp / "preprocessed.ll"
    preprocess_cmd: list[str] = [
        tc.opt,
        f"-load-pass-plugin={env.pass_lib}",
        "-passes=milp-preprocess",
        f"-energy-config={pre_energy_config}",
        f"-milp-config={opts.milp_config}",
        f"-ckpt-log-level={opts.pass_log_level}",
        f"-loop-strip-mining-stats-json={strip_mining_stats_json}",
    ]
    preprocess_cmd += ["-S", str(milp_input_ll), "-o", str(preprocessed_ll)]

    run(preprocess_cmd, step_name="milp-preprocess")

    # Phase 4: Post-strip-mining assembly energy
    post_bb_energy, post_stderr = run_assembly_energy(
        tc,
        env,
        preprocessed_ll,
        tmp / "post",
        opts.energy_config,
        opts.pass_log_level,
        opt_level=opts.opt_level,
    )

    post_energy_config = write_assembly_energy_config(
        tmp / "post_energy_config.json",
        post_bb_energy,
    )

    # Phase 4b: Re-clamp chunked strip-mined loops using post-strip-mining
    # energy so later MILP summarization sees a consistent K.
    reclamped_ll = tmp / "reclamped.ll"
    reclamp_stats_json = tmp / "strip_mining_reclamp_stats.json"
    reclamp_output = _run_loop_reclamp_pass(
        tc,
        env,
        opts,
        energy_config=post_energy_config,
        input_ll=preprocessed_ll,
        output_ll=reclamped_ll,
        strip_mining_stats_json=reclamp_stats_json,
    )
    _merge_strip_mining_reclamp_stats(strip_mining_stats_json, reclamp_stats_json)

    # Phase 5: BB frequency collection
    profile_start = now_ms()

    freq_inst_ll = tmp / "freq_inst.ll"
    run(
        [
            tc.opt,
            f"-load-pass-plugin={env.pass_lib}",
            "-passes=bb-freq-collect-only",
            "-S",
            str(reclamped_ll),
            "-o",
            str(freq_inst_ll),
        ],
        step_name="bb-freq-collect-only",
    )

    bb_freq_json = collect_bb_freq(tc, env, freq_inst_ll, tmp)

    profiling_ms = now_ms() - profile_start

    # Phase 6: MILP solving (on preprocessed IR)
    pass_output = _run_milp_pass(
        tc,
        env,
        opts,
        pass_name="milp-solve-only",
        energy_config=post_energy_config,
        input_ll=reclamped_ll,
        output_ll=tmp / "ckpt.ll",
        bb_freq_json=bb_freq_json,
        milp_extra_flags=milp_extra_flags,
        strip_mining_stats_json=None,
    )

    pass_output = pre_stderr + post_stderr + reclamp_output + pass_output
    return pass_output, profiling_ms, strip_mining_stats_json


# ---------------------------------------------------------------------------
# IR-mode single-pass pipeline
# ---------------------------------------------------------------------------


def _ir_mode(
    tc: Toolchain,
    env: ProjectEnv,
    opts: MilpCompileOptions,
    tmp: Path,
    milp_input_ll: Path,
    milp_extra_flags: list[str],
) -> tuple[str, int, Path]:
    """IR-based single-pass energy estimation pipeline.

    Returns (pass_output, profiling_time_ms).
    """
    # BB frequency collection
    profile_start = now_ms()

    freq_inst_ll = tmp / "freq_inst.ll"
    run(
        [
            tc.opt,
            f"-load-pass-plugin={env.pass_lib}",
            "-passes=bb-freq-collect",
            f"-energy-config={opts.energy_config}",
            f"-milp-config={opts.milp_config}",
            "-S",
            str(milp_input_ll),
            "-o",
            str(freq_inst_ll),
        ],
        step_name="bb-freq-collect",
    )

    bb_freq_json = collect_bb_freq(tc, env, freq_inst_ll, tmp)

    profiling_ms = now_ms() - profile_start

    # MILP pass
    strip_mining_stats_json = tmp / "strip_mining_stats.json"
    pass_output = _run_milp_pass(
        tc,
        env,
        opts,
        pass_name="milp",
        energy_config=opts.energy_config,
        input_ll=milp_input_ll,
        output_ll=tmp / "ckpt.ll",
        bb_freq_json=bb_freq_json,
        milp_extra_flags=milp_extra_flags,
        strip_mining_stats_json=strip_mining_stats_json,
    )

    return pass_output, profiling_ms, strip_mining_stats_json


# ---------------------------------------------------------------------------
# Shared MILP pass invocation
# ---------------------------------------------------------------------------


def _run_milp_pass(
    tc: Toolchain,
    env: ProjectEnv,
    opts: MilpCompileOptions,
    *,
    pass_name: str,
    energy_config: Path,
    input_ll: Path,
    output_ll: Path,
    bb_freq_json: Path,
    milp_extra_flags: list[str],
    strip_mining_stats_json: Path | None,
) -> str:
    """Run a MILP opt pass and return its captured output."""
    cmd: list[str] = [
        tc.opt,
        f"-load-pass-plugin={env.pass_lib}",
        f"-passes={pass_name}",
        f"-energy-config={energy_config}",
        f"-milp-config={opts.milp_config}",
        f"-bb-freq-file={bb_freq_json}",
    ]
    cmd += milp_extra_flags
    if strip_mining_stats_json is not None:
        cmd.append(f"-loop-strip-mining-stats-json={strip_mining_stats_json}")
    if opts.coarse_allocation:
        cmd.append("-milp-coarse-allocation")
    cmd.append(f"-ckpt-stats-json={output_ll.parent / 'stats.json'}")
    cmd += ["-S", str(input_ll), "-o", str(output_ll)]

    result = run(cmd, step_name=pass_name, timeout=660)
    return result.output


def _run_loop_reclamp_pass(
    tc: Toolchain,
    env: ProjectEnv,
    opts: MilpCompileOptions,
    *,
    energy_config: Path,
    input_ll: Path,
    output_ll: Path,
    strip_mining_stats_json: Path,
) -> str:
    """Run the post-strip-mining loop K re-clamp pass."""
    cmd: list[str] = [
        tc.opt,
        f"-load-pass-plugin={env.pass_lib}",
        "-passes=milp-reclamp-only",
        f"-energy-config={energy_config}",
        f"-milp-config={opts.milp_config}",
        f"-ckpt-log-level={opts.pass_log_level}",
        f"-loop-strip-mining-stats-json={strip_mining_stats_json}",
        "-S",
        str(input_ll),
        "-o",
        str(output_ll),
    ]

    result = run(cmd, step_name="milp-reclamp-only")
    return result.output


def _merge_strip_mining_stats(stats_json: Path, strip_mining_stats_json: Path) -> None:
    if not stats_json.is_file():
        return
    if not strip_mining_stats_json.is_file():
        return

    with open(stats_json) as f:
        stats_data = json.load(f)
    with open(strip_mining_stats_json) as f:
        strip_data = json.load(f)

    target_function = stats_data.get("function")
    function_entries = strip_data.get("functions", [])
    matching_entry: dict | None = None
    for entry in function_entries:
        if entry.get("function") == target_function:
            matching_entry = entry
            break
    if matching_entry is None and len(function_entries) == 1:
        matching_entry = function_entries[0]
    if matching_entry is None:
        return

    summary = matching_entry.get("summary")
    if summary is not None:
        stats_data["strip_mining_summary"] = summary
    skipped_reasons = matching_entry.get("skipped_reasons")
    if skipped_reasons is not None:
        stats_data["strip_mining_skipped_reasons"] = skipped_reasons
    chosen_k_values = matching_entry.get("chosen_k_values")
    if chosen_k_values is not None:
        stats_data["strip_mining_chosen_k_values"] = chosen_k_values
    loop_details = matching_entry.get("loop_details")
    if loop_details is not None:
        stats_data["strip_mining_details"] = loop_details

    with open(stats_json, "w") as f:
        json.dump(stats_data, f)


def _merge_strip_mining_reclamp_stats(
    strip_mining_stats_json: Path,
    reclamp_stats_json: Path,
) -> None:
    if not strip_mining_stats_json.is_file():
        return
    if not reclamp_stats_json.is_file():
        return

    with open(strip_mining_stats_json) as f:
        strip_data = json.load(f)
    with open(reclamp_stats_json) as f:
        reclamp_data = json.load(f)

    base_functions = strip_data.get("functions", [])
    reclamp_functions = reclamp_data.get("functions", [])
    if not isinstance(base_functions, list) or not isinstance(reclamp_functions, list):
        return

    base_by_function: dict[str, dict] = {}
    for item in base_functions:
        if not isinstance(item, dict):
            continue
        function_name = item.get("function")
        if isinstance(function_name, str) and function_name:
            base_by_function[function_name] = item

    for reclamp_entry in reclamp_functions:
        if not isinstance(reclamp_entry, dict):
            continue
        function_name = reclamp_entry.get("function")
        if not isinstance(function_name, str) or not function_name:
            continue

        base_entry = base_by_function.get(function_name)
        if (
            base_entry is None
            and len(base_functions) == 1
            and len(reclamp_functions) == 1
        ):
            only_entry = base_functions[0]
            if isinstance(only_entry, dict):
                base_entry = only_entry
        if base_entry is None:
            continue

        base_details = base_entry.get("loop_details", [])
        if not isinstance(base_details, list):
            continue

        base_detail_by_header: dict[str, dict] = {}
        for item in base_details:
            if not isinstance(item, dict):
                continue
            header = item.get("loop_header")
            if isinstance(header, str) and header:
                base_detail_by_header[header] = item

        detail_fields = [
            "decision",
            "skip_reason",
            "skip_detail",
            "chosen_k_valid",
            "chosen_k",
            "post_chunk_reclamp_attempted",
            "post_chunk_reclamp_succeeded",
            "post_chunk_reclamp_applied",
            "post_chunk_max_k_valid",
            "post_chunk_max_k",
            "post_chunk_iter_energy_valid",
            "post_chunk_iter_energy",
            "post_chunk_reclamp_error",
        ]

        for item in reclamp_entry.get("loop_details", []):
            if not isinstance(item, dict):
                continue
            header = item.get("loop_header")
            if not isinstance(header, str) or not header:
                continue
            target = base_detail_by_header.get(header)
            if target is None:
                base_details.append(item)
                base_detail_by_header[header] = item
                continue
            for detail_field in detail_fields:
                if detail_field in item:
                    target[detail_field] = item[detail_field]

        chosen_k_by_header: dict[str, dict] = {}
        for item in base_entry.get("chosen_k_values", []):
            if not isinstance(item, dict):
                continue
            header = item.get("loop_header")
            if isinstance(header, str) and header:
                chosen_k_by_header[header] = item
        for item in reclamp_entry.get("chosen_k_values", []):
            if not isinstance(item, dict):
                continue
            header = item.get("loop_header")
            if isinstance(header, str) and header:
                chosen_k_by_header[header] = item
        base_entry["chosen_k_values"] = [
            chosen_k_by_header[header] for header in sorted(chosen_k_by_header)
        ]

    with open(strip_mining_stats_json, "w") as f:
        json.dump(strip_data, f)


# ---------------------------------------------------------------------------
# MILP link step
# ---------------------------------------------------------------------------


def _link_milp(
    tc: Toolchain,
    env: ProjectEnv,
    opts: MilpCompileOptions,
) -> Path:
    """Assemble and link the MILP output with boot.S + runtime.c."""
    boot_defines = common.build_boot_defines(
        cpu_freq=opts.cpu_freq,
        halt_mode=opts.halt_mode,
        device_debug=opts.device_debug,
    )

    return link_algorithm(
        tc,
        env,
        main_object=opts.output.with_suffix(".o"),
        output_elf=opts.output.with_suffix(".elf"),
        boot_source=env.milp_boot,
        runtime_source=env.milp_runtime,
        linker_script=env.milp_linker,
        boot_defines=boot_defines,
        device_debug=opts.device_debug,
        cpu_freq=opts.cpu_freq,
        gcc_opt_level=opts.opt_level,
    )

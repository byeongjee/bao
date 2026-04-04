"""Unit tests for math-library linking in ckpt compilation helpers."""

from __future__ import annotations

from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace

from ckpt.compile import common, milp, schematic
from ckpt.compile.milp import MilpCompileOptions
from ckpt.compile.schematic import SchematicCompileOptions
from ckpt.runner import StepResult


def test_collect_bb_freq_links_math_library(tmp_path, monkeypatch):
    input_ll = tmp_path / "input.ll"
    input_ll.write_text("; test\n")

    calls: list[tuple[str, list[str], str | None]] = []

    def fake_run(
        cmd: list[str],
        *,
        check: bool = True,
        step_name: str = "",
        cwd: str | None = None,
        timeout: int = 300,
        input: str | None = None,
    ) -> StepResult:
        calls.append((step_name, cmd, cwd))
        if step_name == "bb-freq-run":
            (tmp_path / "bb_freq.json").write_text("{}")
        return StepResult(returncode=0, stdout="", stderr="", duration_ms=0)

    monkeypatch.setattr(common, "run", fake_run)
    monkeypatch.setattr(
        common,
        "strip_ir_for_native",
        lambda _input_ll, output_ll: output_ll.write_text("; native\n"),
    )
    monkeypatch.setattr(
        common,
        "write_native_stubs",
        lambda stubs_c: stubs_c.write_text("void stub(void) {}\n"),
    )

    tc = SimpleNamespace(clang="clang")
    env = SimpleNamespace(
        sysroot_flags=["-isysroot", "/sdk"],
        bb_freq_runtime=Path("/runtime/bb_freq_runtime.c"),
    )

    bb_freq_json = common.collect_bb_freq(tc, env, input_ll, tmp_path)

    assert bb_freq_json == tmp_path / "bb_freq.json"

    compile_cmd = next(cmd for step_name, cmd, _cwd in calls if step_name == "bb-freq-compile")
    assert "-lm" in compile_cmd
    assert compile_cmd[compile_cmd.index("-lm") + 1] == "-o"


def test_assemble_and_link_links_math_library(tmp_path, monkeypatch):
    calls: list[tuple[str, list[str]]] = []

    def fake_run(
        cmd: list[str],
        *,
        check: bool = True,
        step_name: str = "",
        cwd: str | None = None,
        timeout: int = 300,
        input: str | None = None,
    ) -> StepResult:
        del check, cwd, timeout, input
        calls.append((step_name, cmd))
        return StepResult(returncode=0, stdout="", stderr="", duration_ms=0)

    monkeypatch.setattr(common, "run", fake_run)

    tc = SimpleNamespace(gcc="msp430-elf-gcc")
    env = SimpleNamespace(
        device="MSP430FR5994",
        msp430gcc_support_path=Path("/toolchain"),
    )
    objects = [tmp_path / "main.o", tmp_path / "boot.o"]
    output_elf = tmp_path / "out.elf"

    common.assemble_and_link(
        tc,
        env,
        objects,
        output_elf,
        linker_script=tmp_path / "linker.ld",
    )

    link_cmd = next(cmd for step_name, cmd in calls if step_name == "link")
    assert "-lm" in link_cmd
    assert link_cmd[link_cmd.index("-lm") + 1] == "-o"


def test_schematic_trace_compile_links_math_library(tmp_path, monkeypatch):
    calls: list[tuple[str, list[str], str | None]] = []

    def fake_run(
        cmd: list[str],
        *,
        check: bool = True,
        step_name: str = "",
        cwd: str | None = None,
        timeout: int = 300,
        input: str | None = None,
    ) -> StepResult:
        del check, timeout, input
        calls.append((step_name, cmd, cwd))
        if step_name == "trace-run":
            (tmp_path / "schematic_trace.json").write_text("{}")
        return StepResult(returncode=0, stdout="", stderr="", duration_ms=0)

    monkeypatch.setattr(schematic, "run", fake_run)
    monkeypatch.setattr(
        schematic,
        "canonicalize_ir_for_native_profiling",
        lambda _tc, _input_ll, output_ll: output_ll.write_text("; prep\n"),
    )
    monkeypatch.setattr(
        schematic,
        "strip_ir_for_native",
        lambda _input_ll, output_ll: output_ll.write_text("; native\n"),
    )
    monkeypatch.setattr(
        schematic,
        "write_native_stubs",
        lambda stubs_c: stubs_c.write_text("void stub(void) {}\n"),
    )

    tc = SimpleNamespace(clang="clang", opt="opt")
    env = SimpleNamespace(
        pass_lib=Path("/passes/CheckpointPass.so"),
        sysroot_flags=["-isysroot", "/sdk"],
        schematic_trace_runtime=Path("/runtime/schematic_trace_runtime.c"),
    )
    opts = SchematicCompileOptions(
        input_c=tmp_path / "fft.c",
        energy_config=tmp_path / "energy.json",
        schematic_config=tmp_path / "config.json",
        output=tmp_path / "fft",
        estimator_mode="assembly",
        pass_log_level="info",
        debug=False,
        trace_only=False,
        link=False,
        device_debug=False,
        halt_mode="nop",
        cpu_freq=1,
        opt_level=3,
        clang_opt_level=3,
        force_checkpoint_on_incompatible_loops=False,
        recompute_energy_after_new_checkpoint=False,
    )

    trace_json, _profiling_ms = schematic._collect_or_reuse_trace(
        tc,
        env,
        opts,
        tmp_path,
        tmp_path / "input.ll",
    )

    assert trace_json == tmp_path / "schematic_trace.json"

    compile_cmd = next(cmd for step_name, cmd, _cwd in calls if step_name == "trace-compile")
    assert "-lm" in compile_cmd
    assert compile_cmd[compile_cmd.index("-lm") + 1] == "-o"


def test_schematic_pass_forwards_recompute_flag(tmp_path, monkeypatch):
    calls: list[list[str]] = []

    def fake_run(
        cmd: list[str],
        *,
        check: bool = True,
        step_name: str = "",
        cwd: str | None = None,
        timeout: int = 300,
        input: str | None = None,
    ) -> StepResult:
        del check, step_name, cwd, timeout, input
        calls.append(cmd)
        return StepResult(returncode=0, stdout="", stderr="", duration_ms=0)

    monkeypatch.setattr(schematic, "run", fake_run)

    tc = SimpleNamespace(opt="opt")
    env = SimpleNamespace(pass_lib=Path("/passes/CheckpointPass.so"))

    schematic_config = tmp_path / "schematic.json"
    schematic_config.write_text("{}")

    opts = SchematicCompileOptions(
        input_c=tmp_path / "fft.c",
        energy_config=tmp_path / "energy.json",
        schematic_config=schematic_config,
        output=tmp_path / "fft",
        estimator_mode="assembly",
        pass_log_level="info",
        debug=False,
        trace_only=False,
        link=False,
        device_debug=False,
        halt_mode="nop",
        cpu_freq=1,
        opt_level=3,
        clang_opt_level=3,
        force_checkpoint_on_incompatible_loops=False,
        recompute_energy_after_new_checkpoint=False,
    )

    schematic._run_schematic_pass(
        tc,
        env,
        opts,
        tmp_path,
        tmp_path / "input.ll",
        tmp_path / "trace.json",
        energy_config=tmp_path / "energy.json",
    )
    assert "-recompute-energy-after-new-checkpoint" not in calls.pop()

    schematic._run_schematic_pass(
        tc,
        env,
        replace(opts, recompute_energy_after_new_checkpoint=True),
        tmp_path,
        tmp_path / "input.ll",
        tmp_path / "trace.json",
        energy_config=tmp_path / "energy.json",
    )
    assert "-recompute-energy-after-new-checkpoint" in calls.pop()


def test_milp_assembly_mode_runs_choose_strip_mining_k(tmp_path, monkeypatch):
    calls: list[tuple[str, list[str]]] = []
    solve_inputs: list[Path] = []

    def fake_run(
        cmd: list[str],
        *,
        check: bool = True,
        step_name: str = "",
        cwd: str | None = None,
        timeout: int = 300,
        input: str | None = None,
    ) -> StepResult:
        del check, cwd, timeout, input
        calls.append((step_name, cmd))
        if "-o" in cmd:
            Path(cmd[cmd.index("-o") + 1]).write_text("; generated\n")
        return StepResult(returncode=0, stdout="", stderr="", duration_ms=0)

    def fake_run_assembly_energy(_tc, _env, _input_ll, prefix, _energy_config, _log_level):
        bb_energy = Path(f"{prefix}.bb_energy.json")
        bb_energy.write_text("{}")
        return bb_energy, "energy-stderr\n"

    def fake_write_assembly_energy_config(path, _bb_energy):
        path.write_text("{}")
        return path

    def fake_collect_bb_freq(_tc, _env, _freq_inst_ll, tmp):
        bb_freq = tmp / "bb_freq.json"
        bb_freq.write_text("{}")
        return bb_freq

    def fake_run_milp_pass(
        _tc,
        _env,
        _opts,
        *,
        pass_name,
        energy_config,
        input_ll,
        output_ll,
        bb_freq_json,
        milp_extra_flags,
        strip_mining_stats_json,
    ):
        del pass_name, energy_config, bb_freq_json, milp_extra_flags, strip_mining_stats_json
        solve_inputs.append(input_ll)
        output_ll.write_text("; solved\n")
        return ""

    monkeypatch.setattr(milp, "run", fake_run)
    monkeypatch.setattr(milp, "run_assembly_energy", fake_run_assembly_energy)
    monkeypatch.setattr(milp, "write_assembly_energy_config", fake_write_assembly_energy_config)
    monkeypatch.setattr(milp, "collect_bb_freq", fake_collect_bb_freq)
    monkeypatch.setattr(milp, "_run_milp_pass", fake_run_milp_pass)

    tc = SimpleNamespace(opt="opt")
    env = SimpleNamespace(pass_lib=Path("/passes/CheckpointPass.so"))
    input_ll = tmp_path / "input.ll"
    input_ll.write_text("; input\n")
    opts = MilpCompileOptions(
        input_c=tmp_path / "bitcount.c",
        energy_config=tmp_path / "energy.json",
        milp_config=tmp_path / "milp.json",
        output=tmp_path / "bitcount",
        estimator_mode="assembly",
        pass_log_level="info",
        debug=False,
        link=False,
        halt_mode="nop",
        device_debug=False,
        cpu_freq=1,
        opt_level=3,
        clang_opt_level=3,
        milp_gap=0.0,
        milp_log_file="",
        coarse_allocation=False,
        save_temps=False,
        extra_includes=[],
    )

    milp._assembly_mode(tc, env, opts, tmp_path, input_ll, [])

    step_names = [step_name for step_name, _cmd in calls]
    assert step_names == [
        "milp-preprocess",
        "choose-strip-mining-k",
        "bb-freq-collect-only",
    ]

    freq_cmd = next(cmd for step_name, cmd in calls if step_name == "bb-freq-collect-only")
    assert str(tmp_path / "chosen_k.ll") in freq_cmd
    assert solve_inputs == [tmp_path / "chosen_k.ll"]

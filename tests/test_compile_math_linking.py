"""Unit tests for math-library linking in ckpt compilation helpers."""

from __future__ import annotations

from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace

import pytest
from ckpt.compile import common, schematic
from ckpt.compile.schematic import SchematicCompileOptions
from ckpt.runner import StepResult

pytestmark = pytest.mark.unit


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

    compile_cmd = next(
        cmd for step_name, cmd, _cwd in calls if step_name == "bb-freq-compile"
    )
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
        halt_mode=None,
        cpu_freq=1,
        opt_level=3,
        clang_opt_level=3,
        force_checkpoint_on_incompatible_loops=False,
        recompute_energy_after_new_checkpoint=False,
        save_temps=False,
        trace_file=None,
        linker_script=None,
    )

    trace_json, _profiling_ms = schematic._collect_or_reuse_trace(
        tc,
        env,
        opts,
        tmp_path,
        tmp_path / "input.ll",
    )

    assert trace_json == tmp_path / "schematic_trace.json"

    compile_cmd = next(
        cmd for step_name, cmd, _cwd in calls if step_name == "trace-compile"
    )
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
        halt_mode=None,
        cpu_freq=1,
        opt_level=3,
        clang_opt_level=3,
        force_checkpoint_on_incompatible_loops=False,
        recompute_energy_after_new_checkpoint=False,
        save_temps=False,
        trace_file=None,
        linker_script=None,
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

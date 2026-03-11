"""Shared pytest infrastructure for checkpoint insertion tests."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

import pytest

# ---------------------------------------------------------------------------
# Directory layout
# ---------------------------------------------------------------------------
TESTS_DIR = Path(__file__).resolve().parent
PROJECT_DIR = TESTS_DIR.parent
PASS_LIB = PROJECT_DIR / "passes" / "build" / "CheckpointPass.so"
BB_FREQ_RUNTIME = PROJECT_DIR / "passes" / "runtime" / "bb_freq_runtime.c"
SCHEMATIC_TRACE_RUNTIME = PROJECT_DIR / "passes" / "runtime" / "schematic_trace_runtime.c"
SCENARIOS_DIR = TESTS_DIR / "scenarios"
CONFIGS_DIR = SCENARIOS_DIR / "configs"


# ---------------------------------------------------------------------------
# PassResult dataclass
# ---------------------------------------------------------------------------
@dataclass
class PassResult:
    exit_code: int
    stdout: str
    stderr: str
    output_ir: str


# ---------------------------------------------------------------------------
# Tool-path resolution (session scope)
# ---------------------------------------------------------------------------
@pytest.fixture(scope="session")
def tools():
    """Resolve LLVM tool paths and skip all tests if pass library isn't built."""
    env_file = PROJECT_DIR / ".env"
    if env_file.exists():
        for line in env_file.read_text().splitlines():
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, _, v = line.partition("=")
                os.environ.setdefault(k.strip(), v.strip())

    llvm_dir = os.environ.get("LLVM_DIR", "")

    if llvm_dir:
        clang = os.path.join(llvm_dir, "bin", "clang")
        opt = os.path.join(llvm_dir, "bin", "opt")
        llvm_profdata = os.path.join(llvm_dir, "bin", "llvm-profdata")
    else:
        clang = shutil.which("clang") or "clang"
        opt = shutil.which("opt") or "opt"
        llvm_profdata = shutil.which("llvm-profdata") or "llvm-profdata"

    if not PASS_LIB.exists():
        pytest.skip(
            f"Pass library not built: {PASS_LIB}\n"
            "Run: cd passes/build && cmake .. && make",
            allow_module_level=True,
        )

    sysroot_flags: list[str] = []
    if shutil.which("xcrun"):
        try:
            sdk = subprocess.check_output(
                ["xcrun", "--show-sdk-path"], text=True
            ).strip()
            sysroot_flags = ["-isysroot", sdk]
        except subprocess.CalledProcessError:
            pass

    return {
        "clang": clang,
        "opt": opt,
        "llvm_profdata": llvm_profdata,
        "pass_lib": str(PASS_LIB),
        "sysroot_flags": sysroot_flags,
    }


# ---------------------------------------------------------------------------
# Low-level helpers
# ---------------------------------------------------------------------------
def _run(cmd: list[str], *, cwd: str | Path | None = None,
         timeout: int = 120) -> subprocess.CompletedProcess[str]:
    """Run a subprocess, capturing stdout+stderr."""
    return subprocess.run(
        cmd, capture_output=True, text=True, cwd=cwd, timeout=timeout,
    )


# ---------------------------------------------------------------------------
# Pipeline fixtures (return callables)
# ---------------------------------------------------------------------------
@pytest.fixture(scope="session")
def compile_to_ir(tools):
    """Return a callable that compiles C to LLVM IR."""

    def _compile(src: str | Path, out_ll: str | Path, *, mem2reg: bool = False):
        src, out_ll = str(src), str(out_ll)
        cmd = [
            tools["clang"], *tools["sysroot_flags"],
            "-S", "-emit-llvm", "-O0", "-Xclang", "-disable-O0-optnone",
            src, "-o", out_ll,
        ]
        r = _run(cmd)
        if r.returncode != 0:
            raise RuntimeError(f"clang failed: {r.stderr}")
        if mem2reg:
            mem2reg_ll = out_ll.replace(".ll", "_m2r.ll")
            r2 = _run([tools["opt"], "-passes=mem2reg", "-S", out_ll, "-o", mem2reg_ll])
            if r2.returncode != 0:
                raise RuntimeError(f"mem2reg failed: {r2.stderr}")
            shutil.move(mem2reg_ll, out_ll)

    return _compile


@pytest.fixture(scope="session")
def run_milp(tools, compile_to_ir):
    """Return a callable that runs the full MILP pipeline."""

    def _run_milp(
        src: str | Path,
        energy_config: str | Path,
        milp_config: str | Path,
        tmp_path: Path,
        *,
        skip_bb_freq: bool = False,
    ) -> PassResult:
        src = str(src)
        energy_config = str(energy_config)
        milp_config = str(milp_config)

        input_ll = str(tmp_path / "input.ll")
        compile_to_ir(src, input_ll, mem2reg=True)

        bb_freq_flags: list[str] = []
        if not skip_bb_freq:
            # Annotate trip counts
            ann_ll = str(tmp_path / "annotated.ll")
            r = _run([
                tools["opt"], "-load-pass-plugin", tools["pass_lib"],
                "-passes=tripcount-annotation",
                "-S", input_ll, "-o", ann_ll,
            ])
            if r.returncode != 0:
                raise RuntimeError(f"tripcount-annotation failed: {r.stderr}")

            # Instrument for BB frequency collection
            freq_ll = str(tmp_path / "freq_inst.ll")
            r = _run([
                tools["opt"], "-load-pass-plugin", tools["pass_lib"],
                "-passes=bb-freq-collect",
                f"-energy-config={energy_config}",
                f"-milp-config={milp_config}",
                "-S", ann_ll, "-o", freq_ll,
            ])
            if r.returncode != 0:
                raise RuntimeError(f"bb-freq-collect failed: {r.stderr}")

            # Compile and run to get bb_freq.json
            freq_bin = str(tmp_path / "freq_run")
            r = _run([
                tools["clang"], *tools["sysroot_flags"], "-O0",
                freq_ll, str(BB_FREQ_RUNTIME), "-o", freq_bin,
            ])
            if r.returncode != 0:
                raise RuntimeError(f"freq compile failed: {r.stderr}")

            _run([freq_bin], cwd=str(tmp_path))

            bb_freq_json = tmp_path / "bb_freq.json"
            if not bb_freq_json.exists():
                raise RuntimeError("BB frequency collection did not produce bb_freq.json")

            bb_freq_flags = [f"-bb-freq-file={bb_freq_json}"]
            input_ll = ann_ll

        # Run MILP checkpoint pass
        output_ll = str(tmp_path / "output.ll")
        r = _run([
            tools["opt"], "-load-pass-plugin", tools["pass_lib"],
            "-passes=checkpoint",
            f"-energy-config={energy_config}",
            f"-milp-config={milp_config}",
            *bb_freq_flags,
            "-S", input_ll, "-o", output_ll,
        ])

        output_ir = ""
        if os.path.exists(output_ll):
            output_ir = Path(output_ll).read_text()

        return PassResult(
            exit_code=r.returncode,
            stdout=r.stdout,
            stderr=r.stderr,
            output_ir=output_ir,
        )

    return _run_milp


@pytest.fixture(scope="session")
def run_schematic(tools, compile_to_ir):
    """Return a callable that runs the full SCHEMATIC pipeline."""

    def _run_schematic(
        src: str | Path,
        energy_config: str | Path,
        schematic_config: str | Path,
        tmp_path: Path,
    ) -> PassResult:
        src = str(src)
        energy_config = str(energy_config)
        schematic_config = str(schematic_config)
        input_ll = str(tmp_path / "input.ll")
        compile_to_ir(src, input_ll, mem2reg=False)

        # Annotate trip counts
        ann_ll = str(tmp_path / "annotated.ll")
        r = _run([
            tools["opt"], "-load-pass-plugin", tools["pass_lib"],
            "-passes=tripcount-annotation",
            "-S", input_ll, "-o", ann_ll,
        ])
        if r.returncode != 0:
            raise RuntimeError(f"tripcount-annotation failed: {r.stderr}")

        # Instrument for trace collection
        trace_ll = str(tmp_path / "trace_inst.ll")
        r = _run([
            tools["opt"], "-load-pass-plugin", tools["pass_lib"],
            "-passes=trace-collect",
            f"-energy-config={energy_config}",
            "-S", ann_ll, "-o", trace_ll,
        ])
        if r.returncode != 0:
            raise RuntimeError(f"trace-collect failed: {r.stderr}")

        # Compile and run to get schematic_trace.json
        trace_bin = str(tmp_path / "trace_run")
        r = _run([
            tools["clang"], *tools["sysroot_flags"], "-O0",
            trace_ll, str(SCHEMATIC_TRACE_RUNTIME), "-o", trace_bin,
        ])
        if r.returncode != 0:
            raise RuntimeError(f"trace compile failed: {r.stderr}")

        _run([trace_bin], cwd=str(tmp_path))

        trace_json = tmp_path / "schematic_trace.json"
        if not trace_json.exists():
            raise RuntimeError("Trace collection did not produce schematic_trace.json")

        # Run SCHEMATIC pass
        output_ll = str(tmp_path / "output.ll")
        r = _run([
            tools["opt"], "-load-pass-plugin", tools["pass_lib"],
            "-passes=tripcount-annotation,schematic",
            f"-energy-config={energy_config}",
            f"-schematic-config={schematic_config}",
            f"-schematic-trace={trace_json}",
            "-S", ann_ll, "-o", output_ll,
        ])

        output_ir = ""
        if os.path.exists(output_ll):
            output_ir = Path(output_ll).read_text()

        return PassResult(
            exit_code=r.returncode,
            stdout=r.stdout,
            stderr=r.stderr,
            output_ir=output_ir,
        )

    return _run_schematic


@pytest.fixture(scope="session")
def run_schematic_o0(run_schematic):
    """Alias for run_schematic (SCHEMATIC always uses O0/no-mem2reg mode)."""
    return run_schematic


# ---------------------------------------------------------------------------
# IR assertion helpers
# ---------------------------------------------------------------------------
def count_calls(ir: str, func_name: str) -> int:
    """Count 'call ... @func_name(' occurrences in IR."""
    return len(re.findall(rf'call\s+[^@]*@{re.escape(func_name)}\s*\(', ir))


def calls_in_block(ir: str, block_label: str, func_name: str) -> int:
    """Count calls to func_name within a specific basic block."""
    # Find the block: starts with 'label:' and ends at next label or function end
    pattern = rf'^{re.escape(block_label)}:\s*(?:;[^\n]*)?\n(.*?)(?=^\S|\Z)'
    m = re.search(pattern, ir, re.MULTILINE | re.DOTALL)
    if not m:
        return 0
    block_text = m.group(1)
    return len(re.findall(rf'call\s+[^@]*@{re.escape(func_name)}\s*\(', block_text))


def has_global(ir: str, name: str) -> bool:
    """Check if @name = ... exists as a global."""
    return bool(re.search(rf'^@{re.escape(name)}\s*=', ir, re.MULTILINE))


def has_section(ir: str, global_name: str, section: str) -> bool:
    """Check if a global has a specific section annotation."""
    return bool(re.search(
        rf'^@{re.escape(global_name)}\s*=.*section\s+"{re.escape(section)}"',
        ir, re.MULTILINE,
    ))


def get_metric(stderr: str, label: str) -> str | None:
    """Extract 'Label: value' from stderr."""
    m = re.search(rf'^{re.escape(label)}:\s*(.+)$', stderr, re.MULTILINE)
    return m.group(1).strip() if m else None


# ---------------------------------------------------------------------------
# Assertion driver
# ---------------------------------------------------------------------------
def check_assertions(r: PassResult, expect: dict):
    """Validate a PassResult against an expectation dictionary."""

    if "exit" in expect:
        if expect["exit"] == "nonzero":
            assert r.exit_code != 0, (
                f"Expected nonzero exit but got 0.\nstderr: {r.stderr[:500]}"
            )
        else:
            assert r.exit_code == expect["exit"], (
                f"Expected exit={expect['exit']} but got {r.exit_code}.\n"
                f"stderr: {r.stderr[:1000]}"
            )

    ir = r.output_ir

    if "min_boundary" in expect:
        n = count_calls(ir, "__region_boundary")
        assert n >= expect["min_boundary"], (
            f"Expected >= {expect['min_boundary']} __region_boundary calls, got {n}"
        )

    if "max_boundary" in expect:
        n = count_calls(ir, "__region_boundary")
        assert n <= expect["max_boundary"], (
            f"Expected <= {expect['max_boundary']} __region_boundary calls, got {n}"
        )

    # min_prologue/max_prologue/min_epilogue: unified as __region_boundary.
    # MILP now uses a single __region_boundary call at non-entry region starts
    # (no call at the entry node). Old prologue count included the entry node,
    # so boundary count = old prologue count - 1.
    if "min_prologue" in expect:
        n = count_calls(ir, "__region_boundary")
        threshold = expect["min_prologue"] - 1
        assert n >= threshold, (
            f"Expected >= {threshold} __region_boundary calls "
            f"(min_prologue={expect['min_prologue']} - 1), got {n}"
        )

    if "max_prologue" in expect:
        n = count_calls(ir, "__region_boundary")
        threshold = expect["max_prologue"] - 1
        assert n <= threshold, (
            f"Expected <= {threshold} __region_boundary calls "
            f"(max_prologue={expect['max_prologue']} - 1), got {n}"
        )

    if "min_epilogue" in expect:
        # Old epilogue count = non-entry region starts = same as new boundary count
        n = count_calls(ir, "__region_boundary")
        assert n >= expect["min_epilogue"], (
            f"Expected >= {expect['min_epilogue']} __region_boundary calls, got {n}"
        )

    if "no_prologue_in" in expect:
        for label in expect["no_prologue_in"]:
            n = calls_in_block(ir, label, "__region_boundary")
            assert n == 0, (
                f"Expected no __region_boundary in block '{label}', but found {n}"
            )

    if "has_shadow" in expect:
        for name in expect["has_shadow"]:
            shadow_name = f"__vm_shadow_{name}"
            assert has_global(ir, shadow_name), (
                f"Expected shadow global @{shadow_name} not found in IR"
            )

    if "has_nvm_backup" in expect:
        for name in expect["has_nvm_backup"]:
            backup_name = f"__nvm_backup_{name}"
            assert has_global(ir, backup_name), (
                f"Expected NVM backup global @{backup_name} not found in IR"
            )

    if "stderr_contains" in expect:
        assert expect["stderr_contains"] in r.stderr, (
            f"Expected stderr to contain '{expect['stderr_contains']}'\n"
            f"stderr: {r.stderr[:1000]}"
        )

    if "max_shadow_count" in expect:
        count = len(re.findall(r'@__vm_shadow_\w+\s*=\s*internal\s+global', ir))
        assert count <= expect["max_shadow_count"], (
            f"Expected <= {expect['max_shadow_count']} shadow globals, got {count}"
        )

    if "min_shadow_count" in expect:
        count = len(re.findall(r'@__vm_shadow_\w+\s*=\s*internal\s+global', ir))
        assert count >= expect["min_shadow_count"], (
            f"Expected >= {expect['min_shadow_count']} shadow globals, got {count}"
        )

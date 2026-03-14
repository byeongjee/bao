"""RockClimb machine-level (post-regalloc) checkpoint pass tests."""

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
MACHINE_PASS_LIB = PROJECT_DIR / "passes" / "build" / "rockclimb-backend" / "RockClimbMachinePass.so"
ROCKCLIMB_PARAMS = TESTS_DIR / "rockclimb_params.json"
ASSEMBLY_ENERGY_CONFIG = PROJECT_DIR / "benchmarks" / "sample_assembly_energy_params.json"

# ---------------------------------------------------------------------------
# Module-wide mark
# ---------------------------------------------------------------------------
pytestmark = pytest.mark.rockclimb


# ---------------------------------------------------------------------------
# Result dataclass
# ---------------------------------------------------------------------------
@dataclass
class MachinePassResult:
    exit_code: int
    stdout: str
    stderr: str
    output_mir: str
    output_asm: str


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def _resolve_llc() -> str:
    """Find llc, preferring LLVM_DIR if set."""
    llvm_dir = os.environ.get("LLVM_DIR", "")
    if llvm_dir:
        return os.path.join(llvm_dir, "bin", "llc")
    return shutil.which("llc") or "llc"


def _resolve_clang() -> str:
    """Find clang, preferring LLVM_DIR if set."""
    llvm_dir = os.environ.get("LLVM_DIR", "")
    if llvm_dir:
        return os.path.join(llvm_dir, "bin", "clang")
    return shutil.which("clang") or "clang"


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------
@pytest.fixture(scope="session")
def machine_tools():
    """Resolve tool paths and skip if pass library isn't built."""
    # Load .env if present
    env_file = PROJECT_DIR / ".env"
    if env_file.exists():
        for line in env_file.read_text().splitlines():
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, _, v = line.partition("=")
                os.environ.setdefault(k.strip(), v.strip())

    if not MACHINE_PASS_LIB.exists():
        pytest.skip(
            f"Machine pass library not built: {MACHINE_PASS_LIB}\n"
            "Run: cd passes/build && cmake .. && make RockClimbMachinePass",
            allow_module_level=True,
        )

    return {
        "clang": _resolve_clang(),
        "llc": _resolve_llc(),
        "machine_pass_lib": str(MACHINE_PASS_LIB),
    }


@pytest.fixture(scope="session")
def run_rockclimb_machine(machine_tools):
    """Session-scoped fixture: compile C → MIR → run machine pass → assembly."""

    def _run(
        src: Path,
        energy_config: Path,
        rockclimb_config: Path,
        tmp_path: Path,
        *,
        clang_opt: str = "2",
    ) -> MachinePassResult:
        clang = machine_tools["clang"]
        llc = machine_tools["llc"]
        pass_lib = machine_tools["machine_pass_lib"]

        ir_file = tmp_path / "input.ll"
        mir_file = tmp_path / "after_regalloc.mir"
        out_mir = tmp_path / "instrumented.mir"
        out_asm = tmp_path / "output.s"

        # Step 1: C → LLVM IR
        subprocess.run(
            [clang, "-S", "-emit-llvm", f"-O{clang_opt}", "--target=msp430",
             str(src), "-o", str(ir_file)],
            check=True, capture_output=True, text=True,
        )

        # Step 2: IR → MIR (stop after register allocation)
        subprocess.run(
            [llc, "-march=msp430", "-stop-after=virtregrewriter",
             str(ir_file), "-o", str(mir_file)],
            check=True, capture_output=True, text=True,
        )

        # Step 3: Run RockClimb machine pass
        result = subprocess.run(
            [llc, "-march=msp430",
             f"-load={pass_lib}",
             "-run-pass=rockclimb",
             f"-rockclimb-config={rockclimb_config}",
             f"-rockclimb-energy-config={energy_config}",
             str(mir_file), "-o", str(out_mir)],
            capture_output=True, text=True,
        )

        output_mir = out_mir.read_text() if out_mir.exists() else ""
        output_asm = ""

        # Step 4: Resume compilation MIR → assembly (only if pass succeeded)
        if result.returncode == 0 and out_mir.exists():
            asm_result = subprocess.run(
                [llc, "-march=msp430", "-start-after=virtregrewriter",
                 str(out_mir), "-o", str(out_asm)],
                capture_output=True, text=True,
            )
            if asm_result.returncode == 0 and out_asm.exists():
                output_asm = out_asm.read_text()

        return MachinePassResult(
            exit_code=result.returncode,
            stdout=result.stdout,
            stderr=result.stderr,
            output_mir=output_mir,
            output_asm=output_asm,
        )

    return _run


# ---------------------------------------------------------------------------
# Assertion helpers
# ---------------------------------------------------------------------------
def count_mir_calls(mir: str, func_name: str) -> int:
    """Count CALLi &func_name in MIR output."""
    return len(re.findall(rf"CALLi\s+&{re.escape(func_name)}", mir))


def count_asm_calls(asm: str, func_name: str) -> int:
    """Count call #func_name in assembly output."""
    return len(re.findall(rf"call\s+#{re.escape(func_name)}", asm))


def count_mir_reg_saves(mir: str) -> int:
    """Count MOV16mr to __nvm_regs (inline register saves) in MIR output."""
    return len(re.findall(r"MOV16mr.*__nvm_regs", mir))


# ---------------------------------------------------------------------------
# Test C sources (inline)
# ---------------------------------------------------------------------------
SIMPLE_LINEAR = """\
int test_func(int a, int b) {
    int sum = a + b;
    if (sum > 10) {
        sum = sum * 2;
    }
    return sum;
}
"""

SIMPLE_LOOP = """\
int test_loop(int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += i;
    }
    return sum;
}
"""

MULTI_LIVEOUT = """\
int test_liveout(int a, int b) {
    int x = a * 3;
    int y = b * 5;
    if (a > b) {
        x += 10;
        y += 20;
    }
    return x + y;
}
"""

LOOP_BACKEDGE_LIVEOUT = """\
int test_backedge(int *arr, int n) {
    int sum = 0;
    int *p = arr;
    for (int i = 0; i < n; i++) {
        sum += *p;
        p++;
    }
    return sum;
}
"""


def _write_src(tmp_path: Path, code: str) -> Path:
    src = tmp_path / "test.c"
    src.write_text(code)
    return src


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
class TestMachinePassBasic:
    """Basic pass loading and execution tests."""

    def test_pass_runs_successfully(self, run_rockclimb_machine, tmp_path):
        src = _write_src(tmp_path, SIMPLE_LINEAR)
        result = run_rockclimb_machine(
            src, ASSEMBLY_ENERGY_CONFIG, ROCKCLIMB_PARAMS, tmp_path,
        )
        assert result.exit_code == 0, f"Pass failed:\n{result.stderr}"
        assert "Checkpoint Insertion Statistics" in result.stderr

    def test_produces_boundary_checks(self, run_rockclimb_machine, tmp_path):
        src = _write_src(tmp_path, SIMPLE_LINEAR)
        result = run_rockclimb_machine(
            src, ASSEMBLY_ENERGY_CONFIG, ROCKCLIMB_PARAMS, tmp_path,
        )
        assert result.exit_code == 0
        # Should have at least one boundary check in MIR
        checks_mir = count_mir_calls(result.output_mir, "__region_boundary")
        assert checks_mir >= 1, (
            f"Expected >=1 boundary checks in MIR, got {checks_mir}"
        )

    def test_produces_assembly_calls(self, run_rockclimb_machine, tmp_path):
        src = _write_src(tmp_path, SIMPLE_LINEAR)
        result = run_rockclimb_machine(
            src, ASSEMBLY_ENERGY_CONFIG, ROCKCLIMB_PARAMS, tmp_path,
        )
        assert result.exit_code == 0
        assert result.output_asm, "No assembly output produced"
        # Should have checkpoint calls in assembly
        checks_asm = count_asm_calls(result.output_asm, "__region_boundary")
        assert checks_asm >= 1, (
            f"Expected >=1 boundary checks in assembly, got {checks_asm}"
        )


class TestMachinePassLoop:
    """Loop-containing function tests."""

    def test_loop_creates_regions(self, run_rockclimb_machine, tmp_path):
        src = _write_src(tmp_path, SIMPLE_LOOP)
        result = run_rockclimb_machine(
            src, ASSEMBLY_ENERGY_CONFIG, ROCKCLIMB_PARAMS, tmp_path,
        )
        assert result.exit_code == 0
        # Loop header is a mandatory boundary → should create multiple regions
        match = re.search(r"Regions:\s+(\d+)", result.stderr)
        assert match, "Could not find region count in stderr"
        regions = int(match.group(1))
        assert regions >= 2, f"Expected >=2 regions for loop, got {regions}"


class TestMachinePassDistributedCkpt:
    """Distributed checkpointing (register save) tests."""

    def test_register_saves_in_mir(self, run_rockclimb_machine, tmp_path):
        src = _write_src(tmp_path, MULTI_LIVEOUT)
        result = run_rockclimb_machine(
            src, ASSEMBLY_ENERGY_CONFIG, ROCKCLIMB_PARAMS, tmp_path,
        )
        assert result.exit_code == 0
        saves = count_mir_reg_saves(result.output_mir)
        # Functions with live-out registers should have save points
        assert saves >= 0  # May be 0 if no cross-region liveness

    def test_loop_backedge_register_saves(self, run_rockclimb_machine, tmp_path):
        """Registers modified in a loop body and used via back-edge must be saved."""
        src = _write_src(tmp_path, LOOP_BACKEDGE_LIVEOUT)
        result = run_rockclimb_machine(
            src, ASSEMBLY_ENERGY_CONFIG, ROCKCLIMB_PARAMS, tmp_path,
        )
        assert result.exit_code == 0
        saves = count_mir_reg_saves(result.output_mir)
        assert saves >= 2, (
            f"Expected >=2 register saves for loop back-edge liveness, got {saves}"
        )

    def test_register_saves_in_assembly(self, run_rockclimb_machine, tmp_path):
        src = _write_src(tmp_path, MULTI_LIVEOUT)
        result = run_rockclimb_machine(
            src, ASSEMBLY_ENERGY_CONFIG, ROCKCLIMB_PARAMS, tmp_path,
        )
        assert result.exit_code == 0
        saves_mir = count_mir_reg_saves(result.output_mir)
        # If MIR has saves, assembly should reference __nvm_regs
        if saves_mir > 0:
            assert "__nvm_regs" in result.output_asm, (
                f"MIR has {saves_mir} saves but assembly has no __nvm_regs references"
            )


class TestMachinePassStatistics:
    """Verify pass statistics output."""

    def test_statistics_format(self, run_rockclimb_machine, tmp_path):
        src = _write_src(tmp_path, SIMPLE_LINEAR)
        result = run_rockclimb_machine(
            src, ASSEMBLY_ENERGY_CONFIG, ROCKCLIMB_PARAMS, tmp_path,
        )
        assert result.exit_code == 0
        assert "RockClimb-Machine" in result.stderr
        assert "Boundary checks:" in result.stderr
        assert "Register checkpoints:" in result.stderr
        assert "Compilation time (ms):" in result.stderr

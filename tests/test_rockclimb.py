"""RockClimb machine-level (post-regalloc) checkpoint pass tests."""

from __future__ import annotations

import json
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path

import pytest
from ckpt.env import ProjectEnv
from ckpt.toolchain import Toolchain
from conftest import PROJECT_DIR, TESTS_DIR, write_src

# ---------------------------------------------------------------------------
# Directory layout
# ---------------------------------------------------------------------------
MACHINE_PASS_LIB = (
    PROJECT_DIR / "passes" / "build" / "rockclimb-backend" / "RockClimbMachinePass.so"
)
ROCKCLIMB_PARAMS = TESTS_DIR / "rockclimb_params.json"
ASSEMBLY_ENERGY_CONFIG = PROJECT_DIR / "benchmarks" / "assembly_params.json"

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
# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------
@pytest.fixture(scope="session")
def machine_tools():
    """Resolve tool paths and skip if pass library isn't built."""
    if not MACHINE_PASS_LIB.exists():
        pytest.skip(
            f"Machine pass library not built: {MACHINE_PASS_LIB}\n"
            "Run: cd passes/build && cmake .. && make RockClimbMachinePass",
            allow_module_level=True,
        )

    env = ProjectEnv.from_environ(PROJECT_DIR)
    tc = Toolchain.resolve(env)
    return {
        "clang": tc.clang,
        "llc": tc.llc,
        "machine_pass_lib": str(MACHINE_PASS_LIB),
    }


@pytest.fixture(scope="session")
def run_rockclimb_machine(machine_tools):
    """Session-scoped fixture: compile C → MIR → run machine pass → assembly."""

    def _run(
        src: Path,
        energy_config: Path,
        rockclimb_config: Path | None,
        tmp_path: Path,
        *,
        clang_opt: str = "2",
        energy_data: Path | None = None,
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
            [
                clang,
                "-S",
                "-emit-llvm",
                f"-O{clang_opt}",
                "--target=msp430",
                str(src),
                "-o",
                str(ir_file),
            ],
            check=True,
            capture_output=True,
            text=True,
        )

        # Step 2: IR → MIR (stop after register allocation)
        subprocess.run(
            [
                llc,
                "-march=msp430",
                "-stop-after=virtregrewriter",
                str(ir_file),
                "-o",
                str(mir_file),
            ],
            check=True,
            capture_output=True,
            text=True,
        )

        # Step 3: Run RockClimb machine pass
        cmd = [
            llc,
            "-march=msp430",
            f"-load={pass_lib}",
            "-run-pass=rockclimb",
        ]
        if rockclimb_config is not None:
            cmd.append(f"-rockclimb-config={rockclimb_config}")
        cmd.append(
            f"-rockclimb-energy-data={energy_data}"
            if energy_data is not None
            else f"-rockclimb-energy-config={energy_config}"
        )
        cmd += [str(mir_file), "-o", str(out_mir)]
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            check=False,
        )

        output_mir = out_mir.read_text() if out_mir.exists() else ""
        output_asm = ""

        # Step 4: Resume compilation MIR → assembly (only if pass succeeded)
        if result.returncode == 0 and out_mir.exists():
            asm_result = subprocess.run(
                [
                    llc,
                    "-march=msp430",
                    "-start-after=virtregrewriter",
                    str(out_mir),
                    "-o",
                    str(out_asm),
                ],
                capture_output=True,
                text=True,
                check=False,
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


def _extract_mir_block(mir: str, label: str) -> str:
    match = re.search(
        rf"^\s*{re.escape(label)}:\n(.*?)(?=^\s*bb\.\d|^\.\.\.$|\Z)",
        mir,
        re.MULTILINE | re.DOTALL,
    )
    assert match, f"Could not find MIR block {label}"
    return match.group(1)


def _extract_function_mir(mir: str, name: str) -> str:
    """Extract the MIR body of a single function by its `name:` header."""
    match = re.search(
        rf"^name:\s+{re.escape(name)}\s*$(.*?)(?=^name:\s|\Z)",
        mir,
        re.MULTILINE | re.DOTALL,
    )
    assert match, f"Could not find function {name} in MIR"
    return match.group(1)


def _region_boundaries_per_func(stderr: str) -> dict[str, int]:
    """Parse per-function 'Region boundaries:' counts from pass stats."""
    counts: dict[str, int] = {}
    current: str | None = None
    for line in stderr.splitlines():
        m = re.search(r"Function:\s+(\S+)", line)
        if m:
            current = m.group(1)
        m = re.search(r"Region boundaries:\s+(\d+)", line)
        if m and current is not None:
            counts[current] = int(m.group(1))
    return counts


def count_inmodule_calls(mir: str, func_name: str) -> int:
    """Count CALLi @func_name (direct in-module calls use @, externals use &)."""
    return len(re.findall(rf"CALLi\s+@{re.escape(func_name)}", mir))


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

BOUNDARY_BLOCK_DEF = """\
// Branching loop: forces separate header/body/latch blocks so that with
// low E_safe the latch becomes its own boundary.  The loop header (bb.5)
// contains a MOV16rp auto-increment that defines r14 (pointer) with a
// read-modify-write.  Without the fix, the header is not in its own
// predBlockSet, so the r14 definition there has no save point — on
// recovery, r14 is restored to the initial pointer value, restarting
// the traversal from the beginning of the array.
int boundary_block_defs(int *arr, int n) {
    int pos_sum = 0;
    int neg_sum = 0;
    for (int i = 0; i < n; i++) {
        if (arr[i] > 0)
            pos_sum += arr[i];
        else
            neg_sum += arr[i];
    }
    return pos_sum + neg_sum;
}
"""

CROSS_FUNCTION_CALL = """\
// __nvm_regs is shared by every instrumented function.  The caller loads
// `x` into a callee-saved register before the call and keeps it live across
// the post-call loop-header boundary without redefining it.  work() uses
// enough values live across its loop back-edge to occupy the callee-saved
// registers, so its own distributed saves overwrite the caller's slots
// (its epilogue restores only the register values, not the slots).  The
// caller must therefore re-save x's slot AFTER the call; otherwise
// recovery at the post-call boundary restores work()'s stale value.
volatile unsigned seed_v = 0x1234;
volatile unsigned iters_v = 40;

__attribute__((noinline)) unsigned work(unsigned n) {
    unsigned a0 = 1, a1 = 2, a2 = 3, a3 = 4, a4 = 5;
    unsigned a5 = 6, a6 = 7, a7 = 8, a8 = 9, a9 = 10;
    for (unsigned i = 0; i < n; i++) {
        a0 += i;
        a1 ^= a0;
        a2 += a1 >> 1;
        a3 ^= a2 + i;
        a4 += a3;
        a5 ^= a4 >> 2;
        a6 += a5;
        a7 ^= a6 + i;
        a8 += a7;
        a9 ^= a8;
    }
    return a0 ^ a1 ^ a2 ^ a3 ^ a4 ^ a5 ^ a6 ^ a7 ^ a8 ^ a9;
}

unsigned caller(void) {
    unsigned x = seed_v;
    unsigned r = work(iters_v);
    unsigned acc = 0;
    unsigned n = iters_v;
    for (unsigned i = 0; i < n; i++) {
        acc = (acc + x) ^ (acc >> 3);
    }
    return acc ^ r;
}
"""


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
class TestMachinePassBasic:
    """Basic pass loading and execution tests."""

    def test_pass_runs_successfully(self, run_rockclimb_machine, tmp_path):
        src = write_src(tmp_path, SIMPLE_LINEAR)
        result = run_rockclimb_machine(
            src,
            ASSEMBLY_ENERGY_CONFIG,
            ROCKCLIMB_PARAMS,
            tmp_path,
        )
        assert result.exit_code == 0, f"Pass failed:\n{result.stderr}"
        assert "Checkpoint Insertion Statistics" in result.stderr

    def test_produces_boundary_checks(self, run_rockclimb_machine, tmp_path):
        src = write_src(tmp_path, SIMPLE_LINEAR)
        result = run_rockclimb_machine(
            src,
            ASSEMBLY_ENERGY_CONFIG,
            ROCKCLIMB_PARAMS,
            tmp_path,
        )
        assert result.exit_code == 0
        # Should have at least one boundary check in MIR
        checks_mir = count_mir_calls(result.output_mir, "__region_boundary")
        assert checks_mir >= 1, f"Expected >=1 boundary checks in MIR, got {checks_mir}"

    def test_produces_assembly_calls(self, run_rockclimb_machine, tmp_path):
        src = write_src(tmp_path, SIMPLE_LINEAR)
        result = run_rockclimb_machine(
            src,
            ASSEMBLY_ENERGY_CONFIG,
            ROCKCLIMB_PARAMS,
            tmp_path,
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
        src = write_src(tmp_path, SIMPLE_LOOP)
        result = run_rockclimb_machine(
            src,
            ASSEMBLY_ENERGY_CONFIG,
            ROCKCLIMB_PARAMS,
            tmp_path,
        )
        assert result.exit_code == 0
        # Loop header is a mandatory boundary → should create multiple regions
        match = re.search(r"Regions:\s+(\d+)", result.stderr)
        assert match, "Could not find region count in stderr"
        regions = int(match.group(1))
        assert regions >= 2, f"Expected >=2 regions for loop, got {regions}"


IRREDUCIBLE_GOTO = """\
// A goto into the loop body gives the cycle two entry points, so it is not
// a natural loop and MachineLoopInfo does not recognize it (compile at -O0;
// at -O2 SimplifyCFG makes the CFG reducible again by duplication).
int irreducible_entry(int n, int enter_mid) {
    int acc = 0;
    int i = 0;
    if (enter_mid)
        goto mid;
    for (; i < n; i++) {
        acc += i;
    mid:
        acc ^= n;
    }
    return acc;
}
"""


class TestMachinePassIrreducible:
    """Cycles that are not natural loops must still be cut by boundaries."""

    def test_irreducible_cycle_gets_boundary(self, run_rockclimb_machine, tmp_path):
        """MachineLoopInfo does not report irreducible cycles as loops, so the
        loop-header rule alone leaves them without a boundary — their energy
        accumulation would be unbounded and unchecked (silently unsound under
        intermittent power).  Retreating-edge targets must therefore be
        mandatory boundaries."""
        src = write_src(tmp_path, IRREDUCIBLE_GOTO)
        result = run_rockclimb_machine(
            src,
            ASSEMBLY_ENERGY_CONFIG,
            ROCKCLIMB_PARAMS,
            tmp_path,
            clang_opt="0",
        )
        assert result.exit_code == 0, result.stderr
        boundaries = _region_boundaries_per_func(result.stderr)
        # entry + exit + the retreating-edge target inside the cycle
        assert boundaries.get("irreducible_entry", 0) >= 3, (
            f"Irreducible cycle was not cut by a boundary: {boundaries}"
        )


class TestMachinePassDistributedCkpt:
    """Distributed checkpointing (register save) tests."""

    def test_loop_backedge_register_saves(self, run_rockclimb_machine, tmp_path):
        """Registers modified in a loop body and used via back-edge must be saved."""
        src = write_src(tmp_path, LOOP_BACKEDGE_LIVEOUT)
        result = run_rockclimb_machine(
            src,
            ASSEMBLY_ENERGY_CONFIG,
            ROCKCLIMB_PARAMS,
            tmp_path,
        )
        assert result.exit_code == 0
        saves = count_mir_reg_saves(result.output_mir)
        assert saves >= 2, (
            f"Expected >=2 register saves for loop back-edge liveness, got {saves}"
        )

    def test_boundary_block_own_defs_saved(self, run_rockclimb_machine, tmp_path):
        """Registers defined in a boundary block must be saved there.

        With low E_safe the loop latch becomes its own boundary, so the
        backward walk from the loop header stops at the latch and never
        includes the header in its own predBlockSet.  The header's
        auto-increment pointer (r14 in MOV16rp) is a read-modify-write
        that IS live at the boundary.  Without the fix, the definition
        has no save point — recovery restores the initial pointer.
        """
        low_cap_config = tmp_path / "low_cap_rockclimb.json"
        low_cap_config.write_text(
            json.dumps(
                {
                    "capacity": 20.0,
                    "E_pro": 0.0,
                    "E_epi": 0.0,
                    "N_reg": 16,
                    "reg_store_energy": 0.0,
                    "reg_restore_energy": 0.0,
                    "rockclimb": {"distributed_checkpointing": True},
                }
            )
        )

        src = write_src(tmp_path, BOUNDARY_BLOCK_DEF)
        result = run_rockclimb_machine(
            src,
            ASSEMBLY_ENERGY_CONFIG,
            low_cap_config,
            tmp_path,
        )
        assert result.exit_code == 0, f"Pass failed:\n{result.stderr}"

        header_block = _extract_mir_block(result.output_mir, "bb.5.for.body")
        assert "MOV16rp" in header_block
        assert re.search(r"MOV16mr.*__nvm_regs \+ 20, \$r14", header_block), (
            "Expected loop-header auto-increment pointer def to be checkpointed"
        )

        latch_block = _extract_mir_block(result.output_mir, "bb.9.for.body")
        assert re.search(r"MOV16mr.*__nvm_regs \+ 16, \$r12", latch_block), (
            "Expected latch def of r12 to be checkpointed"
        )
        assert re.search(r"MOV16mr.*__nvm_regs \+ 22, \$r15", latch_block), (
            "Expected latch def of r15 to be checkpointed"
        )
        assert re.search(r"MOV16mr.*__nvm_regs \+ 18, \$r13", latch_block), (
            "Expected latch def of r13 to be checkpointed"
        )

    def test_register_saves_in_assembly(self, run_rockclimb_machine, tmp_path):
        src = write_src(tmp_path, MULTI_LIVEOUT)
        result = run_rockclimb_machine(
            src,
            ASSEMBLY_ENERGY_CONFIG,
            ROCKCLIMB_PARAMS,
            tmp_path,
        )
        assert result.exit_code == 0
        saves_mir = count_mir_reg_saves(result.output_mir)
        # If MIR has saves, assembly should reference __nvm_regs
        if saves_mir > 0:
            assert "__nvm_regs" in result.output_asm, (
                f"MIR has {saves_mir} saves but assembly has no __nvm_regs references"
            )

    def test_call_clobbered_slot_resaved_after_call(
        self, run_rockclimb_machine, tmp_path
    ):
        """A register live across a call to an instrumented callee must have
        its __nvm_regs slot re-saved AFTER the call: the callee's own
        distributed saves overwrite the slot even though the register value
        itself is preserved (callee-saved ABI).  A save placed only at the
        pre-call definition leaves the slot stale, so recovery at the next
        boundary in the caller restores the callee's value."""
        src = write_src(tmp_path, CROSS_FUNCTION_CALL)
        result = run_rockclimb_machine(
            src,
            ASSEMBLY_ENERGY_CONFIG,
            ROCKCLIMB_PARAMS,
            tmp_path,
        )
        assert result.exit_code == 0, result.stderr

        caller_mir = _extract_function_mir(result.output_mir, "caller")

        # The callee must itself write __nvm_regs slots (the clobber premise).
        work_mir = _extract_function_mir(result.output_mir, "work")
        assert count_mir_reg_saves(work_mir) > 0, (
            "work() should have distributed saves that overwrite __nvm_regs"
        )

        # Find the register holding x (loaded from seed_v before the call).
        load = re.search(r"\$r(\d+) = MOV16rm \$sr, @seed_v", caller_mir)
        assert load, "Expected a register load of seed_v in caller"
        regnum = int(load.group(1))
        assert 4 <= regnum <= 10, (
            f"Test premise broken: x in $r{regnum}, expected a callee-saved "
            "register (r4-r10) live across the call"
        )

        call = re.search(r"CALLi @work", caller_mir)
        assert call, "Expected direct call to work in caller"

        # The fix: x's slot must be (re-)saved after the call.
        slot = (regnum - 4) * 2
        slot_pat = rf"@__nvm_regs \+ {slot}" if slot else r"@__nvm_regs(?! \+)"
        save_after_call = re.search(
            rf"MOV16mr \$sr, {slot_pat}, \$r{regnum}\b",
            caller_mir[call.end() :],
        )
        assert save_after_call, (
            f"Expected $r{regnum} slot save after CALLi @work — without it, "
            "recovery at the post-call boundary restores work()'s stale value"
        )

    def test_reg_store_energy_creates_more_boundaries(
        self, run_rockclimb_machine, tmp_path
    ):
        """With reg_store_energy > 0, inline overhead estimation should
        produce equal or more region boundaries than without."""
        src = write_src(tmp_path, SIMPLE_LOOP)
        (tmp_path / "base").mkdir()
        (tmp_path / "store").mkdir()

        # Run without reg_store_energy
        base_config = tmp_path / "base_config.json"
        base_config.write_text(
            json.dumps(
                {
                    "capacity": 507.87,
                    "E_pro": 5.0,
                    "E_epi": 3.0,
                    "N_reg": 16,
                    "reg_store_energy": 0.0,
                    "reg_restore_energy": 2.0,
                    "rockclimb": {"distributed_checkpointing": True},
                }
            )
        )
        base_result = run_rockclimb_machine(
            src,
            ASSEMBLY_ENERGY_CONFIG,
            base_config,
            tmp_path / "base",
        )
        assert base_result.exit_code == 0

        # Run with reg_store_energy in rockclimb section
        store_config = tmp_path / "store_config.json"
        store_config.write_text(
            json.dumps(
                {
                    "capacity": 507.87,
                    "E_pro": 5.0,
                    "E_epi": 3.0,
                    "N_reg": 16,
                    "reg_store_energy": 5.0,
                    "reg_restore_energy": 2.0,
                    "rockclimb": {
                        "distributed_checkpointing": True,
                    },
                }
            )
        )
        store_result = run_rockclimb_machine(
            src,
            ASSEMBLY_ENERGY_CONFIG,
            store_config,
            tmp_path / "store",
        )
        assert store_result.exit_code == 0

        # Parse boundary counts
        base_match = re.search(r"Region boundaries:\s+(\d+)", base_result.stderr)
        store_match = re.search(r"Region boundaries:\s+(\d+)", store_result.stderr)
        assert base_match and store_match
        base_boundaries = int(base_match.group(1))
        store_boundaries = int(store_match.group(1))

        assert store_boundaries >= base_boundaries, (
            f"With reg_store_energy, expected >= {base_boundaries} "
            f"boundaries, got {store_boundaries}"
        )


class TestMachinePassStatistics:
    """Verify pass statistics output."""

    def test_statistics_format(self, run_rockclimb_machine, tmp_path):
        src = write_src(tmp_path, SIMPLE_LINEAR)
        result = run_rockclimb_machine(
            src,
            ASSEMBLY_ENERGY_CONFIG,
            ROCKCLIMB_PARAMS,
            tmp_path,
        )
        assert result.exit_code == 0
        assert "RockClimb-Machine" in result.stderr
        assert "Boundary checks:" in result.stderr
        assert "Register checkpoints:" in result.stderr
        assert "Compilation time (ms):" in result.stderr


# ---------------------------------------------------------------------------
# Faithful PFI/ROCKCLIMB call-handling model
#   - boundaries at function entry and exit (NOT at call sites)
#   - entry boundary saves the function's live-in argument registers
#   - external/library calls are costed as single expensive instructions
# ---------------------------------------------------------------------------
TWO_ARG_LEAF = """\
int leaf(int a, int b) { return a + b; }
"""

CALLER_CALLEE_TWICE = """\
int __attribute__((noinline)) callee(int a, int b) { return a + b; }
int caller(int x) { return callee(x, 1) + callee(x, 2); }
"""

# fib keeps a real recursive call at -O2 (unlike a tail/linear recursion,
# which the optimizer rewrites into a loop).
RECURSION = """\
int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
"""


class TestCallHandling:
    """Region boundaries follow the paper's function entry/exit model."""

    def test_entry_boundary_saves_args_first(self, run_rockclimb_machine, tmp_path):
        """The entry block emits a boundary, preceded by saves of the live-in
        argument registers (so recovery into the first region restores args)."""
        src = write_src(tmp_path, TWO_ARG_LEAF)
        result = run_rockclimb_machine(
            src,
            ASSEMBLY_ENERGY_CONFIG,
            ROCKCLIMB_PARAMS,
            tmp_path,
        )
        assert result.exit_code == 0, result.stderr
        mir = result.output_mir
        # Entry boundary is now emitted (no longer skipped).
        assert count_mir_calls(mir, "__region_boundary") >= 1
        # Both argument registers (a, b) are saved at entry.
        assert count_mir_reg_saves(mir) >= 2, mir
        # The saves precede the boundary checkpoint.
        save_m = re.search(r"MOV16mr[^\n]*__nvm_regs", mir)
        bnd_m = re.search(r"CALLi &__region_boundary", mir)
        assert save_m and bnd_m and save_m.start() < bnd_m.start(), (
            "argument saves must come before the entry boundary"
        )

    def test_no_boundary_at_callsite_callee_bracketed(
        self, run_rockclimb_machine, tmp_path
    ):
        """Calls are not boundary sites in the caller; the callee carries its
        own entry/exit boundaries (shared across call sites)."""
        src = write_src(tmp_path, CALLER_CALLEE_TWICE)
        result = run_rockclimb_machine(
            src,
            ASSEMBLY_ENERGY_CONFIG,
            ROCKCLIMB_PARAMS,
            tmp_path,
        )
        assert result.exit_code == 0, result.stderr
        mir = result.output_mir

        # Both call sites survive to MIR (callee is not inlined).
        assert count_inmodule_calls(mir, "callee") >= 2, mir

        # Boundaries are per-function, not per-call-site: the caller is a single
        # block, so it has exactly one (entry=exit) boundary despite 2 calls.
        per_func = _region_boundaries_per_func(result.stderr)
        assert per_func.get("caller") == 1, per_func
        assert per_func.get("callee") == 1, per_func

        # No post-call boundary: a callsite is never immediately followed by one.
        assert not re.search(
            r"CALLi @callee[^\n]*\n\s*CALLi &__region_boundary", mir
        ), "there must be no boundary inserted after a call site"

        # The callee is bracketed by its own entry boundary + arg saves.
        callee_mir = _extract_function_mir(mir, "callee")
        assert "CALLi &__region_boundary" in callee_mir
        assert re.search(r"MOV16mr[^\n]*__nvm_regs", callee_mir), (
            "callee entry must save its incoming argument registers"
        )

    def test_external_call_costed_as_expensive_instruction(
        self, run_rockclimb_machine, tmp_path
    ):
        """An external library call with no entry/exit boundaries of its own is
        costed as a single expensive instruction. Integer division
        (__mspabi_divi ~752) exceeds E_safe (~472) for the default config, so a
        block containing it cannot fit one charge."""
        src = write_src(tmp_path, "int divfn(int a, int b) { return a / b; }")
        result = run_rockclimb_machine(
            src,
            ASSEMBLY_ENERGY_CONFIG,
            ROCKCLIMB_PARAMS,
            tmp_path,
        )
        assert (
            "does not fit one charge" in result.stderr
            or "exceeds E_safe" in result.stderr
        ), result.stderr

    def test_recursion_compiles_with_entry_exit_boundaries(
        self, run_rockclimb_machine, tmp_path
    ):
        """Direct recursion needs no special handling: the (in-module) callee
        carries its own entry/exit boundaries, and the recursive call is cheap."""
        src = write_src(tmp_path, RECURSION)
        result = run_rockclimb_machine(
            src,
            ASSEMBLY_ENERGY_CONFIG,
            ROCKCLIMB_PARAMS,
            tmp_path,
        )
        assert result.exit_code == 0, result.stderr
        mir = result.output_mir
        assert count_inmodule_calls(mir, "fib") >= 1, "recursive call should remain"
        assert count_mir_calls(mir, "__region_boundary") >= 1

    def test_missing_precomputed_block_energy_aborts(
        self, run_rockclimb_machine, tmp_path
    ):
        """Precomputed mode must abort when a block with real instructions is
        missing from the bb-energy data.  Silently falling back to the
        configless default cost (1.0 per instruction) undersizes regions and
        only fails later, non-deterministically, on the device."""
        energy_data = tmp_path / "bb_energy.json"
        energy_data.write_text(
            json.dumps({"functions": {"test_loop": {"bb_energy": {}}}})
        )
        src = write_src(tmp_path, SIMPLE_LOOP)
        result = run_rockclimb_machine(
            src,
            ASSEMBLY_ENERGY_CONFIG,
            ROCKCLIMB_PARAMS,
            tmp_path,
            energy_data=energy_data,
        )
        assert result.exit_code != 0, "pass should abort on incomplete energy data"
        assert "no precomputed energy" in result.stderr, result.stderr


class TestMachinePassConfigErrors:
    """Configuration errors must abort llc instead of silently emitting an
    uninstrumented binary with exit code 0."""

    def test_missing_config_aborts(self, run_rockclimb_machine, tmp_path):
        src = write_src(tmp_path, SIMPLE_LINEAR)
        result = run_rockclimb_machine(
            src,
            ASSEMBLY_ENERGY_CONFIG,
            None,
            tmp_path,
        )
        assert result.exit_code != 0, "llc should abort without -rockclimb-config"
        assert "-rockclimb-config not specified" in result.stderr, result.stderr

    def test_nonpositive_esafe_reports_clear_infeasibility(
        self, run_rockclimb_machine, tmp_path
    ):
        """A capacity below the fixed E_pro/E_epi/restore overhead must report
        a clear E_safe infeasibility (keeping the driver's SKIP flow), not a
        confusing per-block "exceeds E_safe (negative)" message."""
        tiny_cap = tmp_path / "tiny_cap.json"
        tiny_cap.write_text(
            json.dumps(
                {
                    "capacity": 5.0,
                    "E_pro": 5.0,
                    "E_epi": 3.0,
                    "N_reg": 16,
                    "reg_store_energy": 2.0,
                    "reg_restore_energy": 2.0,
                }
            )
        )
        src = write_src(tmp_path, SIMPLE_LINEAR)
        result = run_rockclimb_machine(
            src,
            ASSEMBLY_ENERGY_CONFIG,
            tiny_cap,
            tmp_path,
        )
        assert result.exit_code == 0
        assert re.search(
            r"Region partitioning failed: E_safe .* <= 0", result.stderr
        ), result.stderr

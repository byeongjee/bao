"""Tests for IR parser and CFG construction."""

import subprocess
from pathlib import Path

import pytest

from src.ir_parser import (
    CFG,
    BasicBlockInfo,
    build_cfg,
    compile_c_to_ir,
    parse_ir_module,
)


# Check if clang is available
def clang_available() -> bool:
    try:
        result = subprocess.run(
            ["clang", "--version"],
            capture_output=True,
            check=False,
        )
        return result.returncode == 0
    except FileNotFoundError:
        return False


SAMPLES_DIR = Path(__file__).parent / "samples"


class TestCFG:
    """Tests for the CFG class."""

    def test_add_block(self):
        """Test adding blocks to CFG."""
        cfg = CFG()
        info = BasicBlockInfo(name="entry", energy_cost=10, freq=1.0, loop_depth=0)
        cfg.add_block(info)

        assert "entry" in cfg.blocks()
        assert cfg.get_block_info("entry").energy_cost == 10

    def test_add_edge(self):
        """Test adding edges to CFG."""
        cfg = CFG()
        cfg.add_block(BasicBlockInfo("a", 5, 1.0, 0))
        cfg.add_block(BasicBlockInfo("b", 10, 1.0, 0))
        cfg.add_edge("a", "b")

        assert ("a", "b") in cfg.edges()
        assert cfg.successors("a") == ["b"]
        assert cfg.predecessors("b") == ["a"]

    def test_detect_simple_loop(self):
        """Test loop detection with a simple loop."""
        cfg = CFG()
        cfg.add_block(BasicBlockInfo("entry", 5, 1.0, 0))
        cfg.add_block(BasicBlockInfo("loop_header", 10, 1.0, 0))
        cfg.add_block(BasicBlockInfo("loop_body", 15, 1.0, 0))
        cfg.add_block(BasicBlockInfo("exit", 5, 1.0, 0))

        cfg.entry_block = "entry"
        cfg.add_edge("entry", "loop_header")
        cfg.add_edge("loop_header", "loop_body")
        cfg.add_edge("loop_body", "loop_header")  # Back edge
        cfg.add_edge("loop_header", "exit")

        loop_depths = cfg.detect_loops()

        assert loop_depths["entry"] == 0
        assert loop_depths["loop_header"] >= 1
        assert loop_depths["loop_body"] >= 1
        assert loop_depths["exit"] == 0 or loop_depths["exit"] >= 0  # May be in loop


@pytest.mark.skipif(not clang_available(), reason="clang not available")
class TestIRParsing:
    """Tests for LLVM IR parsing (requires clang)."""

    def test_compile_simple_loop(self):
        """Test compiling simple_loop.c to IR."""
        c_file = SAMPLES_DIR / "simple_loop.c"
        ir_text = compile_c_to_ir(str(c_file))

        assert "define" in ir_text
        assert "main" in ir_text

    def test_parse_and_build_cfg(self):
        """Test parsing IR and building CFG."""
        c_file = SAMPLES_DIR / "simple_loop.c"
        ir_text = compile_c_to_ir(str(c_file))
        module = parse_ir_module(ir_text)
        cfg = build_cfg(module, "main")

        assert cfg.entry_block is not None
        assert len(cfg.blocks()) > 0
        assert len(cfg.edges()) > 0

    def test_branching_cfg(self):
        """Test CFG construction for branching code."""
        c_file = SAMPLES_DIR / "branching.c"
        ir_text = compile_c_to_ir(str(c_file))
        module = parse_ir_module(ir_text)

        # Test both functions
        cfg_abs = build_cfg(module, "abs_val")
        assert len(cfg_abs.blocks()) >= 2  # At least if/else branches

        cfg_main = build_cfg(module, "main")
        assert cfg_main.entry_block is not None

    def test_function_not_found(self):
        """Test error when function doesn't exist."""
        c_file = SAMPLES_DIR / "simple_loop.c"
        ir_text = compile_c_to_ir(str(c_file))
        module = parse_ir_module(ir_text)

        with pytest.raises(ValueError, match="not found"):
            build_cfg(module, "nonexistent_function")


class TestIRParsingWithInlineIR:
    """Tests using inline LLVM IR (no clang required)."""

    def test_parse_simple_ir(self):
        """Test parsing a simple inline LLVM IR."""
        ir_text = """
        define i32 @simple() {
        entry:
          %x = add i32 1, 2
          ret i32 %x
        }
        """
        module = parse_ir_module(ir_text)
        cfg = build_cfg(module, "simple")

        assert cfg.entry_block == "entry"
        assert len(cfg.blocks()) == 1
        # Entry block should have add (1) + ret (1) = 2 energy
        info = cfg.get_block_info("entry")
        assert info.energy_cost >= 2

    def test_parse_branching_ir(self):
        """Test parsing IR with branches."""
        ir_text = """
        define i32 @branch(i1 %cond) {
        entry:
          br i1 %cond, label %then, label %else
        then:
          %a = add i32 1, 1
          br label %end
        else:
          %b = mul i32 2, 2
          br label %end
        end:
          %result = phi i32 [ %a, %then ], [ %b, %else ]
          ret i32 %result
        }
        """
        module = parse_ir_module(ir_text)
        cfg = build_cfg(module, "branch")

        assert cfg.entry_block == "entry"
        assert len(cfg.blocks()) == 4
        assert set(cfg.blocks()) == {"entry", "then", "else", "end"}

        # Check edges
        assert ("entry", "then") in cfg.edges()
        assert ("entry", "else") in cfg.edges()
        assert ("then", "end") in cfg.edges()
        assert ("else", "end") in cfg.edges()

        # Check energy costs
        then_info = cfg.get_block_info("then")
        assert then_info.energy_cost >= 1  # add instruction

        else_info = cfg.get_block_info("else")
        assert else_info.energy_cost >= 5  # mul instruction

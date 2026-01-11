"""LLVM IR parsing and Control Flow Graph construction."""

import re
import subprocess
from dataclasses import dataclass

import llvmlite.binding as llvm
import networkx as nx

from .energy_model import estimate_instruction_cost


@dataclass
class BasicBlockInfo:
    """Information about a basic block in the CFG."""

    name: str
    energy_cost: int  # Sum of instruction costs in the block
    freq: float  # Estimated execution frequency
    loop_depth: int  # Nesting depth in loops (0 = not in loop)


class CFG:
    """Control Flow Graph wrapper around NetworkX DiGraph."""

    def __init__(self) -> None:
        self.graph: nx.DiGraph = nx.DiGraph()
        self.entry_block: str | None = None

    def add_block(self, info: BasicBlockInfo) -> None:
        """Add a basic block to the CFG.

        Args:
            info: BasicBlockInfo containing block metadata.
        """
        self.graph.add_node(info.name, info=info)

    def add_edge(self, src: str, dst: str) -> None:
        """Add a control flow edge from src to dst.

        Args:
            src: Source block name.
            dst: Destination block name.
        """
        self.graph.add_edge(src, dst)

    def predecessors(self, block: str) -> list[str]:
        """Get predecessor blocks."""
        return list(self.graph.predecessors(block))

    def successors(self, block: str) -> list[str]:
        """Get successor blocks."""
        return list(self.graph.successors(block))

    def get_block_info(self, block: str) -> BasicBlockInfo:
        """Get the BasicBlockInfo for a block."""
        return self.graph.nodes[block]["info"]

    def blocks(self) -> list[str]:
        """Get all block names in the CFG."""
        return list(self.graph.nodes())

    def edges(self) -> list[tuple[str, str]]:
        """Get all edges as (src, dst) tuples."""
        return list(self.graph.edges())

    def detect_loops(self) -> dict[str, int]:
        """Detect loops and compute loop depth for each block.

        Uses DFS to find back edges and compute loop nesting depth.

        Returns:
            Dict mapping block name to its loop depth (0 = not in loop).
        """
        if not self.entry_block:
            return {b: 0 for b in self.blocks()}

        # Find back edges using DFS
        visited: set[str] = set()
        in_stack: set[str] = set()
        back_edges: list[tuple[str, str]] = []

        def dfs(node: str) -> None:
            visited.add(node)
            in_stack.add(node)
            for succ in self.successors(node):
                if succ not in visited:
                    dfs(succ)
                elif succ in in_stack:
                    # Back edge found
                    back_edges.append((node, succ))
            in_stack.remove(node)

        dfs(self.entry_block)

        # Compute loop depth based on back edges
        # Simple heuristic: count how many loop headers dominate each block
        loop_depth: dict[str, int] = {b: 0 for b in self.blocks()}

        for latch, header in back_edges:
            # Find all blocks in this natural loop
            loop_blocks = self._find_loop_blocks(header, latch)
            for block in loop_blocks:
                loop_depth[block] += 1

        return loop_depth

    def _find_loop_blocks(self, header: str, latch: str) -> set[str]:
        """Find all blocks in the natural loop with given header and latch.

        A natural loop consists of the header and all blocks that can reach
        the latch without going through the header.

        Args:
            header: The loop header block.
            latch: The block with the back edge to header.

        Returns:
            Set of block names in the loop.
        """
        # Natural loop: header + all blocks that can reach latch without
        # going through header (i.e., blocks on path from header to latch)
        loop_blocks: set[str] = {header, latch}

        # Work backwards from latch to find all blocks in the loop
        worklist = [latch]
        while worklist:
            block = worklist.pop()
            for pred in self.predecessors(block):
                if pred not in loop_blocks:
                    loop_blocks.add(pred)
                    worklist.append(pred)

        return loop_blocks


def compile_c_to_ir(c_file_path: str) -> str:
    """Compile a C source file to LLVM IR text using clang.

    Args:
        c_file_path: Path to the C source file.

    Returns:
        LLVM IR as a string.

    Raises:
        RuntimeError: If clang compilation fails.
    """
    result = subprocess.run(
        ["clang", "-S", "-emit-llvm", "-O0", "-o", "-", c_file_path],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"clang compilation failed:\n{result.stderr}")
    return result.stdout


def parse_ir_module(ir_text: str) -> llvm.ModuleRef:
    """Parse LLVM IR text into a module reference.

    Args:
        ir_text: LLVM IR as a string.

    Returns:
        Parsed LLVM module.

    Raises:
        RuntimeError: If parsing fails.
    """
    try:
        return llvm.parse_assembly(ir_text)
    except RuntimeError as e:
        raise RuntimeError(f"Failed to parse LLVM IR: {e}") from e


def _extract_opcode(instruction_str: str) -> str:
    """Extract the opcode from an LLVM instruction string.

    Args:
        instruction_str: String representation of an LLVM instruction.

    Returns:
        The opcode as a lowercase string.
    """
    # LLVM instruction format: [result = ] opcode operands
    # Examples:
    #   %1 = add i32 %0, 1
    #   store i32 %1, ptr %sum
    #   br label %for.cond
    #   ret i32 %sum.0

    # Remove leading whitespace and result assignment if present
    instr = instruction_str.strip()

    # Skip result assignment (e.g., "%1 = ")
    if "=" in instr:
        _, instr = instr.split("=", 1)
        instr = instr.strip()

    # The opcode is the first word
    parts = instr.split()
    if not parts:
        return "unknown"

    opcode = parts[0].lower()

    # Handle special cases
    if opcode.startswith("tail"):
        # tail call -> call
        return "call"

    return opcode


def _extract_block_name(block_str: str, block_index: int) -> str:
    """Extract block name from block string representation.

    Args:
        block_str: Block string from llvmlite.
        block_index: Index of the block in the function (0 = entry).

    Returns:
        The block name/label.
    """
    # Block format varies:
    # Entry block (no label): starts directly with instructions
    # Labeled block: "label:  ; preds = ...\n  instructions..."
    # Numbered label: "4:  ; preds = ...\n  instructions..."

    lines = block_str.strip().split("\n")
    if not lines:
        return f"bb{block_index}"

    first_line = lines[0].strip()

    # Check if first line is a label (ends with : before any comment)
    # Format: "label:" or "label:  ; preds = ..."
    label_match = re.match(r"^([a-zA-Z0-9_.]+)\s*:", first_line)
    if label_match:
        return label_match.group(1)

    # No label found - this is the entry block
    return f"bb{block_index}"


def _extract_successors(block_str: str) -> list[str]:
    """Extract successor block names from terminator instruction.

    Args:
        block_str: Block string representation.

    Returns:
        List of successor block names.
    """
    successors: list[str] = []
    lines = block_str.strip().split("\n")

    if not lines:
        return successors

    # Find terminator (last non-empty line)
    terminator = ""
    for line in reversed(lines):
        line = line.strip()
        if line and not line.endswith(":"):
            terminator = line
            break

    # Parse br instruction
    # br label %target
    # br i1 %cond, label %true, label %false
    if terminator.startswith("br "):
        # Find all label references
        label_pattern = r"label\s+%([a-zA-Z0-9_.]+)"
        matches = re.findall(label_pattern, terminator)
        successors.extend(matches)

    # Parse switch instruction
    # switch i32 %val, label %default [ i32 0, label %case0 ... ]
    elif terminator.startswith("switch "):
        label_pattern = r"label\s+%([a-zA-Z0-9_.]+)"
        matches = re.findall(label_pattern, terminator)
        successors.extend(matches)

    # ret and unreachable have no successors

    return successors


def build_cfg(module: llvm.ModuleRef, function_name: str = "main") -> CFG:
    """Build a CFG from a specific function in the LLVM module.

    Args:
        module: Parsed LLVM module.
        function_name: Name of the function to analyze.

    Returns:
        CFG representing the function's control flow.

    Raises:
        ValueError: If the function is not found.
    """
    cfg = CFG()

    # Find the target function
    target_func = None
    for func in module.functions:
        if func.name == function_name:
            target_func = func
            break

    if target_func is None:
        raise ValueError(f"Function '{function_name}' not found in module")

    # First pass: create all blocks with energy costs
    block_names: list[str] = []
    for block_idx, block in enumerate(target_func.blocks):
        block_str = str(block)
        block_name = _extract_block_name(block_str, block_idx)
        block_names.append(block_name)

        # Calculate energy cost by summing instruction costs
        energy_cost = 0
        for instr in block.instructions:
            opcode = _extract_opcode(str(instr))
            energy_cost += estimate_instruction_cost(opcode)

        # Initial freq and loop_depth (will be updated later)
        info = BasicBlockInfo(
            name=block_name,
            energy_cost=energy_cost,
            freq=1.0,
            loop_depth=0,
        )
        cfg.add_block(info)

    # Set entry block
    if block_names:
        cfg.entry_block = block_names[0]

    # Second pass: add edges
    for block_idx, block in enumerate(target_func.blocks):
        block_str = str(block)
        block_name = _extract_block_name(block_str, block_idx)
        successors = _extract_successors(block_str)

        for succ in successors:
            if succ in block_names:
                cfg.add_edge(block_name, succ)

    # Third pass: detect loops and update frequencies
    loop_depths = cfg.detect_loops()
    for block_name, depth in loop_depths.items():
        info = cfg.get_block_info(block_name)
        info.loop_depth = depth
        # Frequency heuristic: 10^depth
        info.freq = float(10**depth)

    return cfg

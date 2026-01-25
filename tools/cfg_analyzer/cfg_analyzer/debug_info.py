"""Parse LLVM IR debug information for source line mapping.

This module extracts debug information from LLVM IR files compiled with -g flag,
enabling per-loop bound mapping by correlating source line numbers.
"""

import re
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class IRDebugInfo:
    """Debug information extracted from LLVM IR."""

    # Maps basic block names to their first instruction's source line
    # e.g., {"for.cond": 16, "for.body": 17, "5": 20}
    block_to_line: dict[str, int] = field(default_factory=dict)

    # Maps annotation source lines to bound values
    # e.g., {15: 100, 28: 32}
    annotation_line_to_bound: dict[int, int] = field(default_factory=dict)

    # Maps !dbg metadata IDs to (line, column) tuples
    # e.g., {"24": (16, 5), "28": (17, 9)}
    metadata_to_location: dict[str, tuple[int, int]] = field(default_factory=dict)

    # Per-function block-to-line mappings (since block names can repeat across functions)
    # e.g., {"sum_array": {"entry": 12, "for.cond": 16}, "nested": {...}}
    function_block_to_line: dict[str, dict[str, int]] = field(default_factory=dict)


def parse_debug_info(ir_path: Path) -> IRDebugInfo:
    """Parse debug info from LLVM IR file compiled with -g flag.

    Extracts:
    1. Source line numbers from DILocation metadata
    2. LOOP_BOUND annotation lines from llvm.var.annotation calls
    3. Block-to-line mappings from !dbg references

    Args:
        ir_path: Path to .ll file (must be compiled with -g flag)

    Returns:
        IRDebugInfo with extracted mappings
    """
    info = IRDebugInfo()

    ir_content = ir_path.read_text()

    # Step 1: Parse all !DILocation metadata
    # Pattern: !24 = !DILocation(line: 16, column: 5, scope: !10)
    di_location_pattern = re.compile(
        r"!(\d+)\s*=\s*!DILocation\(line:\s*(\d+),\s*column:\s*(\d+)"
    )
    for match in di_location_pattern.finditer(ir_content):
        meta_id = match.group(1)
        line = int(match.group(2))
        column = int(match.group(3))
        info.metadata_to_location[meta_id] = (line, column)

    # Step 2: Parse annotation strings to get bound values
    # Pattern: @.str = ... c"loop_bound:10\00"
    str_to_bound: dict[str, int] = {}
    bound_string_pattern = re.compile(
        r"(@\.str(?:\.\d+)?)\s*=.*?c\"loop_bound:(\d+)\\00\""
    )
    for match in bound_string_pattern.finditer(ir_content):
        str_name = match.group(1)
        bound = int(match.group(2))
        str_to_bound[str_name] = bound

    # Step 3: Parse llvm.var.annotation calls to get source lines
    # Pattern: call void @llvm.var.annotation...(ptr %var, ptr @.str.N, ptr @file, i32 LINE, ...)
    annotation_pattern = re.compile(
        r"call void @llvm\.var\.annotation[^(]*\([^,]+,\s*ptr[^@]*(@\.str(?:\.\d+)?)[^,]*,[^,]+,\s*i32\s+(\d+)"
    )
    for match in annotation_pattern.finditer(ir_content):
        str_ref = match.group(1)
        line = int(match.group(2))
        if str_ref in str_to_bound:
            info.annotation_line_to_bound[line] = str_to_bound[str_ref]

    # Step 4: Parse block-to-line mappings from instructions with !dbg
    # We need to find the first !dbg reference in each basic block
    # Parse per-function to handle repeated block names across functions
    info.function_block_to_line = _extract_block_source_lines_per_function(
        ir_content, info.metadata_to_location
    )

    # Also provide a flat (legacy) mapping - takes last occurrence for duplicates
    for func_blocks in info.function_block_to_line.values():
        info.block_to_line.update(func_blocks)

    return info


def _extract_block_source_lines_per_function(
    ir_content: str,
    metadata_to_location: dict[str, tuple[int, int]],
) -> dict[str, dict[str, int]]:
    """Extract source line numbers for each basic block, organized by function.

    Parses the IR to find basic block labels and their first instruction's
    !dbg metadata reference, keeping track of which function each block belongs to.

    Args:
        ir_content: Full text of the .ll file
        metadata_to_location: Pre-parsed !DILocation mappings

    Returns:
        Dict mapping function names to their block-to-line dicts
    """
    function_blocks: dict[str, dict[str, int]] = {}
    current_function: str | None = None
    current_block: str | None = None
    current_block_has_line = False

    # Pattern for block labels: either named (for.cond:) or numbered (5:)
    block_label_pattern = re.compile(r"^([a-zA-Z_][a-zA-Z0-9_.]*|\d+):\s*(?:;.*)?$")

    # Pattern for !dbg metadata reference
    dbg_pattern = re.compile(r"!dbg\s+!(\d+)")

    # Pattern to extract function name from define line
    # define ... @function_name(...)
    func_name_pattern = re.compile(r"define\s+[^@]*@([a-zA-Z_][a-zA-Z0-9_]*)\s*\(")

    for line in ir_content.split("\n"):
        stripped = line.strip()

        # Check for function definition
        if stripped.startswith("define "):
            func_match = func_name_pattern.search(stripped)
            if func_match:
                current_function = func_match.group(1)
                function_blocks[current_function] = {}

            current_block = "entry"  # First block in function is entry
            current_block_has_line = False

            # Check if define line itself has !dbg
            if current_function:
                dbg_match = dbg_pattern.search(stripped)
                if dbg_match:
                    meta_id = dbg_match.group(1)
                    if meta_id in metadata_to_location:
                        function_blocks[current_function]["entry"] = metadata_to_location[meta_id][0]
                        current_block_has_line = True
            continue

        # Check for function end
        if stripped == "}" and current_function:
            current_function = None
            current_block = None
            continue

        # Check for basic block label
        block_match = block_label_pattern.match(stripped)
        if block_match:
            current_block = block_match.group(1)
            current_block_has_line = False
            continue

        # Check for !dbg reference in instruction
        if current_function and current_block and not current_block_has_line:
            dbg_match = dbg_pattern.search(stripped)
            if dbg_match:
                meta_id = dbg_match.group(1)
                if meta_id in metadata_to_location:
                    src_line = metadata_to_location[meta_id][0]
                    # Skip line 0 (compiler-generated)
                    if src_line > 0:
                        function_blocks[current_function][current_block] = src_line
                        current_block_has_line = True

    return function_blocks


def parse_annotation_lines(ir_path: Path) -> dict[int, int]:
    """Convenience function to just get annotation lines and bounds.

    Args:
        ir_path: Path to .ll file

    Returns:
        Dict mapping source line numbers to bound values
    """
    info = parse_debug_info(ir_path)
    return info.annotation_line_to_bound

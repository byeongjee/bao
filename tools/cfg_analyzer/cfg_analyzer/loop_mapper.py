"""Map loops to their LOOP_BOUND annotations.

This module correlates loop back-edges with their corresponding LOOP_BOUND
annotations by matching source line numbers.
"""

from dataclasses import dataclass, field

import networkx as nx

from .debug_info import IRDebugInfo


@dataclass
class LoopInfo:
    """Information about a single loop."""

    # The back-edge that defines this loop (from_block, to_block)
    back_edge: tuple[str, str]

    # Source line of the loop header (target of back-edge)
    header_line: int | None

    # The bound value from the nearest preceding LOOP_BOUND annotation
    bound: int | None

    # Source line of the annotation (for debugging/reporting)
    annotation_line: int | None


@dataclass
class LoopBoundMapping:
    """Complete mapping of loops to bounds for a function."""

    function_name: str

    # All loops identified in the CFG
    loops: list[LoopInfo] = field(default_factory=list)

    # Quick lookup: back_edge -> bound (or None if no annotation)
    edge_to_bound: dict[tuple[str, str], int | None] = field(default_factory=dict)

    # Default bound for loops without annotations
    default_bound: int = 2


def find_back_edges(cfg: nx.DiGraph, entry: str) -> list[tuple[str, str]]:
    """Find all back-edges in the CFG using DFS.

    A back-edge is an edge from a node to one of its ancestors in the DFS tree,
    indicating a loop.

    Args:
        cfg: NetworkX directed graph representing the CFG
        entry: Entry node of the CFG

    Returns:
        List of (source, target) tuples representing back-edges
    """
    if entry not in cfg:
        return []

    back_edges: list[tuple[str, str]] = []
    visited: set[str] = set()
    rec_stack: set[str] = set()

    def dfs(node: str) -> None:
        visited.add(node)
        rec_stack.add(node)
        for succ in cfg.successors(node):
            if succ in rec_stack:
                back_edges.append((node, succ))
            elif succ not in visited:
                dfs(succ)
        rec_stack.remove(node)

    dfs(entry)
    return back_edges


def match_annotation_to_loop(
    loop_header_line: int,
    annotation_lines: dict[int, int],
) -> tuple[int | None, int | None]:
    """Find the nearest preceding LOOP_BOUND annotation for a loop.

    The matching rule is: find the annotation on the line immediately before
    the loop header, or the closest preceding annotation if no immediate one.

    Args:
        loop_header_line: Source line of the loop header
        annotation_lines: {source_line: bound_value}

    Returns:
        (bound_value, annotation_line) or (None, None) if no match
    """
    # Find annotations on lines strictly before the loop header
    candidates = [
        (line, bound)
        for line, bound in annotation_lines.items()
        if line < loop_header_line
    ]

    if not candidates:
        return None, None

    # Return the one closest to the loop header
    nearest = max(candidates, key=lambda x: x[0])
    return nearest[1], nearest[0]


def map_loops_to_bounds(
    cfg: nx.DiGraph,
    entry: str,
    debug_info: IRDebugInfo,
    node_to_block: dict[str, str] | None = None,
    default_bound: int = 2,
    function_name: str = "",
) -> LoopBoundMapping:
    """Map each loop back-edge to its LOOP_BOUND annotation.

    Algorithm:
    1. Identify all back-edges via DFS from entry
    2. For each back-edge target (loop header), get its source line
    3. Find the nearest LOOP_BOUND annotation preceding that line
    4. Associate the bound with the back-edge

    Args:
        cfg: NetworkX directed graph representing the CFG
        entry: Entry node of the CFG
        debug_info: Parsed debug information from IR
        node_to_block: Optional mapping from CFG node names to IR block names
                       (needed because DOT files use memory addresses as node names)
        default_bound: Default bound for loops without annotations
        function_name: Name of the function (for reporting)

    Returns:
        LoopBoundMapping with all loop-to-bound associations
    """
    mapping = LoopBoundMapping(
        function_name=function_name,
        default_bound=default_bound,
    )

    # Find all back-edges
    back_edges = find_back_edges(cfg, entry)

    # Use function-specific block-to-line mapping if available
    block_to_line = debug_info.function_block_to_line.get(
        function_name, debug_info.block_to_line
    )

    for source, target in back_edges:
        # Get the IR block name for the loop header
        if node_to_block:
            block_name = node_to_block.get(target, target)
        else:
            block_name = target

        # Get the source line of the loop header
        header_line = block_to_line.get(block_name)

        # Find matching annotation
        bound = None
        annotation_line = None
        if header_line is not None:
            bound, annotation_line = match_annotation_to_loop(
                header_line,
                debug_info.annotation_line_to_bound,
            )

        # Create loop info
        loop_info = LoopInfo(
            back_edge=(source, target),
            header_line=header_line,
            bound=bound,
            annotation_line=annotation_line,
        )
        mapping.loops.append(loop_info)

        # Add to edge lookup (use bound or None to indicate default should be used)
        mapping.edge_to_bound[(source, target)] = bound

    return mapping


def get_effective_bound(
    mapping: LoopBoundMapping,
    edge: tuple[str, str],
) -> int:
    """Get the effective bound for a back-edge.

    Returns the annotated bound if available, otherwise the default bound.

    Args:
        mapping: The loop bound mapping
        edge: The back-edge (source, target)

    Returns:
        The bound to use for this edge
    """
    bound = mapping.edge_to_bound.get(edge)
    if bound is not None:
        return bound
    return mapping.default_bound

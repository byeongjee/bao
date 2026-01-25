"""Path counting algorithms for CFGs.

The fundamental challenge: CFGs with loops have infinite paths.
We provide multiple counting strategies.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from typing import TYPE_CHECKING

import networkx as nx

if TYPE_CHECKING:
    from .loop_mapper import LoopBoundMapping


class PathCountStrategy(Enum):
    """Strategy for counting paths in CFGs with loops."""

    ACYCLIC = "acyclic"  # Count paths that don't revisit nodes
    BOUNDED = "bounded"  # Count paths with max N loop iterations
    COLLAPSED = "collapsed"  # Collapse loops to single nodes, then count


@dataclass
class PathCountResult:
    """Result of path counting."""

    strategy: PathCountStrategy
    count: int
    details: dict
    # Per-loop bounds used (if applicable): {back_edge: bound}
    loop_bounds_used: dict[tuple[str, str], int] = field(default_factory=dict)


def count_paths(
    cfg: nx.DiGraph,
    entry: str,
    exits: list[str],
    strategy: PathCountStrategy = PathCountStrategy.BOUNDED,
    max_iterations: int = 2,
) -> PathCountResult:
    """Count paths using the specified strategy."""
    if strategy == PathCountStrategy.ACYCLIC:
        count = count_acyclic_paths(cfg, entry, exits)
        details = {"description": "Simple paths (no node revisits)"}
    elif strategy == PathCountStrategy.BOUNDED:
        count = count_bounded_loop_paths(cfg, entry, exits, max_iterations)
        details = {
            "description": f"Paths with max {max_iterations} loop iterations",
            "max_iterations": max_iterations,
        }
    else:  # COLLAPSED
        count = count_loop_collapsed_paths(cfg, entry, exits)
        details = {"description": "Paths with loops collapsed to single nodes"}

    return PathCountResult(strategy=strategy, count=count, details=details)


def count_acyclic_paths(cfg: nx.DiGraph, entry: str, exits: list[str]) -> int:
    """Count all simple (acyclic) paths from entry to any exit.

    A simple path visits each node at most once.
    This is finite even for graphs with loops.
    """
    if not exits:
        return 0

    if cfg.number_of_nodes() == 0:
        return 0

    # Check if the graph is acyclic
    if nx.is_directed_acyclic_graph(cfg):
        return _count_dag_paths(cfg, entry, exits)

    # For cyclic graphs, count simple paths
    return _count_simple_paths_memo(cfg, entry, exits)


def _count_dag_paths(cfg: nx.DiGraph, entry: str, exits: list[str]) -> int:
    """Count paths in a DAG using dynamic programming.

    path_count[v] = number of paths from v to any exit
    path_count[exit] = 1
    path_count[v] = sum(path_count[succ] for succ in successors(v))

    Process in reverse topological order for O(V + E) complexity.
    """
    exit_set = set(exits)
    path_count: dict[str, int] = {exit_node: 1 for exit_node in exits}

    for node in reversed(list(nx.topological_sort(cfg))):
        if node not in path_count:
            path_count[node] = sum(
                path_count.get(succ, 0) for succ in cfg.successors(node)
            )

    return path_count.get(entry, 0)


def _count_simple_paths_memo(cfg: nx.DiGraph, entry: str, exits: list[str]) -> int:
    """Count simple paths in a graph with cycles.

    Uses memoization with visited set represented as frozenset.
    Warning: Exponential in worst case, but practical for small CFGs.
    """
    exit_set = frozenset(exits)
    memo: dict[tuple[str, frozenset[str]], int] = {}

    def count_from(node: str, visited: frozenset[str]) -> int:
        if node in exit_set:
            return 1

        key = (node, visited)
        if key in memo:
            return memo[key]

        total = 0
        for succ in cfg.successors(node):
            if succ not in visited:
                total += count_from(succ, visited | {succ})

        memo[key] = total
        return total

    return count_from(entry, frozenset({entry}))


def count_bounded_loop_paths(
    cfg: nx.DiGraph,
    entry: str,
    exits: list[str],
    max_iterations: int = 2,
) -> int:
    """Count paths where each loop back-edge can be traversed at most max_iterations times.

    Uses dynamic programming with memoization to avoid exponential recomputation.
    """
    if not exits:
        return 0

    if cfg.number_of_nodes() == 0:
        return 0

    exit_set = frozenset(exits)

    # Check if entry exists in graph
    if entry not in cfg:
        return 0

    # Identify back edges (edges that go to an ancestor in DFS tree)
    back_edges: set[tuple[str, str]] = set()
    back_edge_list: list[tuple[str, str]] = []
    visited: set[str] = set()
    rec_stack: set[str] = set()

    def find_back_edges(node: str) -> None:
        visited.add(node)
        rec_stack.add(node)
        for succ in cfg.successors(node):
            if succ in rec_stack:
                back_edges.add((node, succ))
                back_edge_list.append((node, succ))
            elif succ not in visited:
                find_back_edges(succ)
        rec_stack.remove(node)

    try:
        find_back_edges(entry)
    except RecursionError:
        # Graph too deep, fall back to acyclic counting
        return count_acyclic_paths(cfg, entry, exits)

    # If no back edges, use efficient DAG counting
    if not back_edges:
        return _count_dag_paths(cfg, entry, exits)

    # Compute loop body nodes for each back-edge target (loop header)
    loop_body_nodes: dict[str, frozenset[str]] = {}
    for src, tgt in back_edges:
        reachable_from_tgt = set(nx.descendants(cfg, tgt)) | {tgt}
        can_reach_src = set(nx.ancestors(cfg, src)) | {src}
        loop_nodes = frozenset(reachable_from_tgt & can_reach_src)
        loop_body_nodes[tgt] = loop_nodes

    # Create index for back edges for efficient tuple creation
    back_edge_index = {edge: i for i, edge in enumerate(back_edge_list)}

    # Iterative DP using explicit stack to avoid recursion limits
    # State: (node, visited, edge_counts, phase, accumulated_count, children_to_process)
    # phase 0 = first visit (push children), phase 1 = collecting results

    memo: dict[tuple, int] = {}
    initial_counts = tuple([0] * len(back_edge_list))

    # Stack entries: (node, visited, edge_counts, return_to, child_index)
    # return_to is the key of the parent state waiting for this result
    stack: list[tuple] = [(entry, frozenset({entry}), initial_counts, None, 0)]
    results: dict[tuple, int] = {}  # Store intermediate results
    pending: dict[tuple, list[tuple]] = {}  # Children pending for each state

    while stack:
        node, visited, edge_counts, parent_key, _ = stack.pop()
        key = (node, visited, edge_counts)

        # Check memo - if already fully computed, use cached result
        if key in memo:
            if parent_key is not None:
                results[parent_key] = results.get(parent_key, 0) + memo[key]
            continue

        # Exit node
        if node in exit_set:
            memo[key] = 1
            if parent_key is not None:
                results[parent_key] = results.get(parent_key, 0) + 1
            continue

        # First time visiting - find children to process
        if key not in pending:
            pending[key] = []
            results[key] = 0

            for succ in cfg.successors(node):
                edge = (node, succ)

                if edge in back_edges:
                    idx = back_edge_index[edge]
                    current_count = edge_counts[idx]
                    if current_count >= max_iterations:
                        continue

                    new_counts = list(edge_counts)
                    new_counts[idx] = current_count + 1

                    loop_nodes = loop_body_nodes.get(succ, frozenset())
                    new_visited = (visited - loop_nodes) | {succ}

                    child_key = (succ, new_visited, tuple(new_counts))
                    pending[key].append(child_key)
                elif succ not in visited:
                    child_key = (succ, visited | {succ}, edge_counts)
                    pending[key].append(child_key)

        # If there are still children to process, push self and next child
        if pending[key]:
            child = pending[key].pop()
            stack.append((node, visited, edge_counts, parent_key, 0))
            stack.append((child[0], child[1], child[2], key, 0))
        else:
            # All children processed, finalize this node
            memo[key] = results.get(key, 0)
            if parent_key is not None:
                results[parent_key] = results.get(parent_key, 0) + memo[key]

    return memo.get((entry, frozenset({entry}), initial_counts), 0)


def count_loop_collapsed_paths(cfg: nx.DiGraph, entry: str, exits: list[str]) -> int:
    """Collapse each SCC (loop) into a single node, then count paths in resulting DAG.

    This gives a "structural" path count ignoring loop iterations.
    """
    if not exits:
        return 0

    if cfg.number_of_nodes() == 0:
        return 0

    # Get SCCs and build condensation graph
    # condensation returns a DAG where each node is an SCC
    condensation = nx.condensation(cfg)

    # Find which condensation node contains entry and exits
    # The condensation graph has a 'mapping' attribute: original node -> condensation node
    mapping = condensation.graph.get("mapping", {})

    if entry not in mapping:
        return 0

    cond_entry = mapping[entry]
    cond_exits = list(set(mapping[e] for e in exits if e in mapping))

    if not cond_exits:
        return 0

    return _count_dag_paths(condensation, cond_entry, cond_exits)


def count_bounded_loop_paths_per_loop(
    cfg: nx.DiGraph,
    entry: str,
    exits: list[str],
    loop_mapping: LoopBoundMapping,
) -> PathCountResult:
    """Count paths where each back-edge uses its specific loop bound.

    This is the per-loop variant of count_bounded_loop_paths, using
    LOOP_BOUND annotations to set different bounds for different loops.

    Args:
        cfg: NetworkX directed graph representing the CFG
        entry: Entry node of the CFG
        exits: List of exit nodes
        loop_mapping: Mapping from back-edges to their bounds

    Returns:
        PathCountResult with count and details about bounds used
    """
    if not exits:
        return PathCountResult(
            strategy=PathCountStrategy.BOUNDED,
            count=0,
            details={"description": "No exits"},
        )

    if cfg.number_of_nodes() == 0:
        return PathCountResult(
            strategy=PathCountStrategy.BOUNDED,
            count=0,
            details={"description": "Empty graph"},
        )

    if entry not in cfg:
        return PathCountResult(
            strategy=PathCountStrategy.BOUNDED,
            count=0,
            details={"description": "Entry not in graph"},
        )

    exit_set = frozenset(exits)

    # Use pre-computed back-edges from loop_mapping
    back_edges = set(loop_mapping.edge_to_bound.keys())
    back_edge_list = list(back_edges)

    # If no back edges, use efficient DAG counting
    if not back_edges:
        return PathCountResult(
            strategy=PathCountStrategy.BOUNDED,
            count=_count_dag_paths(cfg, entry, exits),
            details={"description": "No loops (DAG)"},
        )

    # Compute loop body nodes for each back-edge target (loop header)
    loop_body_nodes: dict[str, frozenset[str]] = {}
    for src, tgt in back_edges:
        reachable_from_tgt = set(nx.descendants(cfg, tgt)) | {tgt}
        can_reach_src = set(nx.ancestors(cfg, src)) | {src}
        loop_nodes = frozenset(reachable_from_tgt & can_reach_src)
        loop_body_nodes[tgt] = loop_nodes

    # Create index for back edges
    back_edge_index = {edge: i for i, edge in enumerate(back_edge_list)}

    # Track which bounds were actually used
    bounds_used: dict[tuple[str, str], int] = {}

    def get_bound(edge: tuple[str, str]) -> int:
        """Get the bound for an edge, using annotation or default."""
        bound = loop_mapping.edge_to_bound.get(edge)
        if bound is not None:
            return bound
        return loop_mapping.default_bound

    # Pre-compute max iterations for each back edge
    max_iters = tuple(get_bound(edge) for edge in back_edge_list)
    for i, edge in enumerate(back_edge_list):
        bounds_used[edge] = max_iters[i]

    # Iterative DP using explicit stack to avoid recursion limits
    memo: dict[tuple, int] = {}
    initial_counts = tuple([0] * len(back_edge_list))

    # Stack entries: (node, visited, edge_counts, parent_key, child_index)
    stack: list[tuple] = [(entry, frozenset({entry}), initial_counts, None, 0)]
    results: dict[tuple, int] = {}  # Store intermediate results
    pending: dict[tuple, list[tuple]] = {}  # Children pending for each state

    while stack:
        node, visited, edge_counts, parent_key, _ = stack.pop()
        key = (node, visited, edge_counts)

        # Check memo - if already fully computed, use cached result
        if key in memo:
            if parent_key is not None:
                results[parent_key] = results.get(parent_key, 0) + memo[key]
            continue

        # Exit node
        if node in exit_set:
            memo[key] = 1
            if parent_key is not None:
                results[parent_key] = results.get(parent_key, 0) + 1
            continue

        # First time visiting - find children to process
        if key not in pending:
            pending[key] = []
            results[key] = 0

            for succ in cfg.successors(node):
                edge = (node, succ)

                if edge in back_edges:
                    idx = back_edge_index[edge]
                    current_count = edge_counts[idx]
                    if current_count >= max_iters[idx]:
                        continue

                    new_counts = list(edge_counts)
                    new_counts[idx] = current_count + 1

                    loop_nodes = loop_body_nodes.get(succ, frozenset())
                    new_visited = (visited - loop_nodes) | {succ}

                    child_key = (succ, new_visited, tuple(new_counts))
                    pending[key].append(child_key)
                elif succ not in visited:
                    child_key = (succ, visited | {succ}, edge_counts)
                    pending[key].append(child_key)

        # If there are still children to process, push self and next child
        if pending[key]:
            child = pending[key].pop()
            stack.append((node, visited, edge_counts, parent_key, 0))
            stack.append((child[0], child[1], child[2], key, 0))
        else:
            # All children processed, finalize this node
            memo[key] = results.get(key, 0)
            if parent_key is not None:
                results[parent_key] = results.get(parent_key, 0) + memo[key]

    count = memo.get((entry, frozenset({entry}), initial_counts), 0)

    # Build details about per-loop bounds
    annotated_loops = [
        (edge, bound)
        for edge, bound in loop_mapping.edge_to_bound.items()
        if bound is not None
    ]

    return PathCountResult(
        strategy=PathCountStrategy.BOUNDED,
        count=count,
        details={
            "description": "Paths with per-loop bounds",
            "default_bound": loop_mapping.default_bound,
            "annotated_loops": len(annotated_loops),
            "total_loops": len(loop_mapping.loops),
        },
        loop_bounds_used=bounds_used,
    )

"""CFG data structures and basic analysis."""

from dataclasses import dataclass

import networkx as nx


@dataclass
class CFGStats:
    """Statistics for a function's CFG."""

    function_name: str
    num_basic_blocks: int
    num_edges: int
    has_loops: bool
    num_loops: int
    entry_block: str
    exit_blocks: list[str]


class FunctionCFG:
    """Represents a single function's control flow graph."""

    def __init__(self, name: str, graph: nx.DiGraph):
        self.name = name
        self.graph = graph
        self._identify_entry_exit()

    def _identify_entry_exit(self) -> None:
        """Identify entry block (no predecessors) and exit blocks (no successors)."""
        # Entry: nodes with no incoming edges
        entries = [n for n in self.graph.nodes() if self.graph.in_degree(n) == 0]
        self.entry = entries[0] if entries else list(self.graph.nodes())[0]

        # Exit: nodes with no outgoing edges
        self.exits = [n for n in self.graph.nodes() if self.graph.out_degree(n) == 0]

    def _has_self_loop(self, node: str) -> bool:
        """Check if a node has a self-loop."""
        return self.graph.has_edge(node, node)

    def get_loops(self) -> list[set[str]]:
        """Get all loops (SCCs with more than one node or self-loops)."""
        loops = []
        for scc in nx.strongly_connected_components(self.graph):
            if len(scc) > 1:
                loops.append(scc)
            elif len(scc) == 1:
                node = next(iter(scc))
                if self._has_self_loop(node):
                    loops.append(scc)
        return loops

    def get_stats(self) -> CFGStats:
        """Compute CFG statistics."""
        loops = self.get_loops()

        return CFGStats(
            function_name=self.name,
            num_basic_blocks=self.graph.number_of_nodes(),
            num_edges=self.graph.number_of_edges(),
            has_loops=len(loops) > 0,
            num_loops=len(loops),
            entry_block=self.entry,
            exit_blocks=self.exits,
        )

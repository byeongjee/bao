"""Parse LLVM-generated DOT files into NetworkX graphs."""

from pathlib import Path

import networkx as nx
import pydot


def parse_dot_file(dot_path: Path) -> nx.DiGraph:
    """Parse DOT file into NetworkX directed graph."""
    graphs = pydot.graph_from_dot_file(str(dot_path))

    if not graphs:
        raise ValueError(f"Failed to parse DOT file: {dot_path}")

    graph = graphs[0]

    G = nx.DiGraph()

    for node in graph.get_nodes():
        node_name = node.get_name().strip('"')
        # Skip special nodes
        if node_name in ("node", "edge", "graph"):
            continue
        label = node.get_label() or node_name
        G.add_node(node_name, label=label)

    for edge in graph.get_edges():
        src = edge.get_source().strip('"')
        dst = edge.get_destination().strip('"')
        # Strip port notation (e.g., "Node0x123:s0" -> "Node0x123")
        if ":" in src:
            src = src.split(":")[0]
        if ":" in dst:
            dst = dst.split(":")[0]
        G.add_edge(src, dst)

    return G


def extract_node_to_block_mapping(graph: nx.DiGraph) -> dict[str, str]:
    """Extract mapping from DOT node names to IR block names.

    LLVM DOT files use memory addresses as node IDs (e.g., Node0x102c538b0)
    but the actual block names are in the labels (e.g., "entry:", "for.cond:").

    Args:
        graph: NetworkX graph with node labels

    Returns:
        Dict mapping node IDs to block names
    """
    import re

    node_to_block: dict[str, str] = {}

    # Pattern to extract block name from label: {block_name:\l|...}
    block_pattern = re.compile(r"^\{?([a-zA-Z_][a-zA-Z0-9_.]*|\d+):")

    for node in graph.nodes():
        label = graph.nodes[node].get("label", "")
        if label:
            # Remove surrounding quotes if present
            label = label.strip('"')
            match = block_pattern.search(label)
            if match:
                block_name = match.group(1)
                node_to_block[node] = block_name

    return node_to_block


def extract_function_name(dot_path: Path) -> str:
    """Extract function name from DOT file path.

    LLVM generates files like '.main.dot' for function 'main'.
    """
    name = dot_path.stem
    # Remove leading dot if present
    if name.startswith("."):
        name = name[1:]
    return name

"""CFG visualization using Graphviz."""

import re
import subprocess
from pathlib import Path

import networkx as nx


def visualize_cfg(
    cfg: nx.DiGraph,
    function_name: str,
    output_path: Path,
    format: str = "png",
    highlight_loops: bool = True,
) -> Path:
    """Generate CFG visualization using graphviz library.

    Args:
        cfg: NetworkX directed graph
        function_name: Name for the graph title
        output_path: Where to save the output
        format: Output format (png, svg, pdf)
        highlight_loops: Color loop nodes differently

    Returns:
        Path to generated image
    """
    import graphviz

    dot = graphviz.Digraph(
        name=f"CFG_{function_name}",
        comment=f"Control Flow Graph for {function_name}",
    )
    dot.attr(rankdir="TB")  # Top to bottom
    dot.attr("node", shape="box", fontname="Courier", fontsize="10")

    # Identify loops for highlighting
    loop_nodes: set[str] = set()
    if highlight_loops:
        for scc in nx.strongly_connected_components(cfg):
            if len(scc) > 1:
                loop_nodes.update(scc)
            elif len(scc) == 1:
                node = next(iter(scc))
                if cfg.has_edge(node, node):
                    loop_nodes.add(node)

    # Find entry and exit nodes
    entry_nodes = {n for n in cfg.nodes() if cfg.in_degree(n) == 0}
    exit_nodes = {n for n in cfg.nodes() if cfg.out_degree(n) == 0}

    # Add nodes
    for node in cfg.nodes():
        label = _extract_block_name(cfg.nodes[node].get("label", node))

        attrs = {}

        # Color based on node type
        if node in entry_nodes:
            attrs["fillcolor"] = "lightgreen"
            attrs["style"] = "filled"
        elif node in exit_nodes:
            attrs["fillcolor"] = "lightcoral"
            attrs["style"] = "filled"
        elif node in loop_nodes:
            attrs["fillcolor"] = "lightyellow"
            attrs["style"] = "filled"

        dot.node(node, label, **attrs)

    # Add edges
    for src, dst in cfg.edges():
        dot.edge(src, dst)

    # Render
    output_file = output_path.with_suffix("")
    dot.render(str(output_file), format=format, cleanup=True)

    return output_path


def _extract_block_name(label: str) -> str:
    """Extract just the block name from LLVM's verbose label."""
    if not label:
        return "unknown"

    # LLVM labels often look like:
    # "{entry:\l  %0 = alloca i32\l  store i32 %x, i32* %0\l}"
    # We just want "entry"

    # Remove curly braces and get first line
    label = label.strip('"{}')

    # Get the first line which contains the label
    first_line = label.split("\\l")[0].strip()

    # Remove trailing colon if present
    if first_line.endswith(":"):
        first_line = first_line[:-1]

    # If it's just a number, prefix with "BB"
    if first_line.isdigit():
        first_line = f"BB{first_line}"

    return first_line if first_line else "unknown"


def use_llvm_dot_directly(dot_path: Path, output_path: Path, format: str = "png") -> Path:
    """Use LLVM-generated DOT file directly with Graphviz.

    This preserves LLVM's styling and IR content in nodes.
    """
    cmd = ["dot", f"-T{format}", str(dot_path), "-o", str(output_path)]
    subprocess.run(cmd, check=True)

    return output_path

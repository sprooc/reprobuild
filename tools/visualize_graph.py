#!/usr/bin/env python3
"""Visualize build_graph.yaml as an SVG / PNG using Graphviz."""

import argparse
import os
import sys

import yaml

try:
    import graphviz
except ImportError:
    sys.exit("Please install graphviz Python bindings: pip install graphviz")


# ── appearance ────────────────────────────────────────────────────────────────

NODE_STYLE = {
    "source":       dict(shape="ellipse",  style="filled", fillcolor="#AED6F1", color="#1A5276", width="0.8", height="0.6"),
    "intermediate": dict(shape="box",      style="filled", fillcolor="#FAD7A0", color="#784212", width="0.8", height="0.5"),
    "artifact":     dict(shape="circle", style="filled", fillcolor="#A9DFBF", color="#1E8449", width="0.7", height="0.7", penwidth="2.5"),
    "unknown":      dict(shape="diamond",  style="filled", fillcolor="#D5D8DC", color="#566573", width="0.6", height="0.6"),
}

CMD_COLOR = {
    "gcc":    "#1A5276",
    "g++":    "#1A5276",
    "cc":     "#1A5276",
    "c++":    "#1A5276",
    "clang":  "#154360",
    "clang++":"#154360",
    "as":     "#6E2F8A",
    "ar":     "#784212",
    "ranlib": "#784212",
    "ld":     "#922B21",
    "ld.bfd": "#922B21",
    "ld.gold":"#922B21",
    "ld.lld": "#922B21",
    "make":   "#145A32",
}


def shorten(path: str, max_len: int = 48, wrap: bool = False) -> str:
    """Keep the last two components of a path and trim if still long."""
    if not path:
        return "(none)"
    parts = path.replace("\\", "/").split("/")
    short = "/".join(parts[-2:]) if len(parts) > 2 else path
    if len(short) > max_len:
        if wrap and len(short) > max_len * 1.5:
            # wrap long paths into multiple lines
            mid = len(short) // 2
            return short[:mid] + "\n" + short[mid:]
        short = "…" + short[-(max_len - 1):]
    return short


def node_label(node: dict, max_len: int = 48, show_hash: bool = True, wrap: bool = False) -> str:
    path = node.get("path", "")
    h = node.get("hash", "")
    h_short = h[:10] + "…" if h and len(h) > 10 else (h or "")
    label = shorten(path, max_len, wrap)
    if h_short and show_hash:
        label += f"\n[{h_short}]"
    return label


def safe_id(text: str) -> str:
    """Turn an arbitrary string into a stable Graphviz node id."""
    return str(abs(hash(text)))


def build_dot(data: dict, show_tmp: bool, show_cmake_scratch: bool, 
              max_label_len: int = 48, show_hash: bool = True, wrap_labels: bool = False) -> graphviz.Digraph:
    dot = graphviz.Digraph(
        name="build_graph",
        graph_attr=dict(
            rankdir="LR",
            splines="ortho",
            nodesep="0.4",
            ranksep="1.2",
            fontname="Helvetica",
            bgcolor="white",
        ),
        node_attr=dict(fontname="Helvetica", fontsize="10"),
        edge_attr=dict(fontname="Helvetica", fontsize="9"),
    )

    nodes: dict[str, dict] = {}
    for n in data.get("nodes", []):
        path = n.get("path", "")
        # optional filters
        if not show_tmp and path.startswith("/tmp/"):
            continue
        if not show_cmake_scratch and "CMakeScratch" in path:
            continue
        nodes[path] = n

    # ── file nodes ────────────────────────────────────────────────────────────
    for path, node in nodes.items():
        ntype = node.get("type", "unknown")
        style = NODE_STYLE.get(ntype, NODE_STYLE["unknown"])
        dot.node(
            safe_id(path),
            label=node_label(node, max_label_len, show_hash, wrap_labels),
            tooltip=path,
            **style,
        )

    # ── edge nodes (one per build-tool invocation) + arrows ───────────────────
    for idx, edge in enumerate(data.get("edges", [])):
        cmd = edge.get("command", "?")
        cmd_path = edge.get("command_path", "")
        output = edge.get("output", "")
        inputs = edge.get("inputs") or []
        pid = edge.get("pid", "")

        # skip if output is not in our filtered node set
        if output and output not in nodes and not (
            not show_tmp and output.startswith("/tmp/")
        ):
            pass  # keep going – edge may still be useful

        color = CMD_COLOR.get(cmd, "#566573")

        # diamond node representing the command invocation
        eid = f"edge_{idx}_{pid}"
        dot.node(
            eid,
            label=f"{cmd}\n(pid {pid})",
            shape="diamond",
            style="filled",
            fillcolor=color,
            fontcolor="white",
            color=color,
            fontsize="9",
            width="0.9",
            height="0.5",
            tooltip=cmd_path,
        )

        # input file → command
        for inp in inputs:
            if inp not in nodes:
                continue
            dot.edge(safe_id(inp), eid, color=color, arrowsize="0.7")

        # command → output file
        if output and output in nodes:
            dot.edge(eid, safe_id(output), color=color, arrowsize="0.7",
                     style="bold")

    return dot


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Visualize a reprobuild build_graph.yaml"
    )
    ap.add_argument(
        "yaml_file",
        nargs="?",
        default="build_graph.yaml",
        help="Path to build_graph.yaml (default: build_graph.yaml)",
    )
    ap.add_argument(
        "-o", "--output",
        default="build_graph",
        help="Output file base name (default: build_graph)",
    )
    ap.add_argument(
        "-f", "--format",
        default="svg",
        choices=["svg", "png", "pdf"],
        help="Output format (default: svg)",
    )
    ap.add_argument(
        "--show-tmp",
        action="store_true",
        help="Include /tmp/*.s intermediate assembly files",
    )
    ap.add_argument(
        "--show-cmake-scratch",
        action="store_true",
        help="Include CMake scratch / compiler detection nodes",
    )
    ap.add_argument(
        "--max-label-len",
        type=int,
        default=48,
        help="Maximum length for node labels (default: 48)",
    )
    ap.add_argument(
        "--no-hash",
        action="store_true",
        help="Hide file hash values to make nodes smaller",
    )
    ap.add_argument(
        "--wrap-labels",
        action="store_true",
        help="Wrap long labels across multiple lines",
    )
    args = ap.parse_args()

    if not os.path.exists(args.yaml_file):
        sys.exit(f"File not found: {args.yaml_file}")

    with open(args.yaml_file, encoding="utf-8") as f:
        data = yaml.safe_load(f)

    dot = build_dot(data, args.show_tmp, args.show_cmake_scratch,
                    args.max_label_len, not args.no_hash, args.wrap_labels)
    out_path = dot.render(
        filename=args.output,
        format=args.format,
        cleanup=True,
    )
    print(f"Written: {out_path}")

    # ── legend ────────────────────────────────────────────────────────────────
    print("\nLegend:")
    print("  Ellipse  (blue)   – source file")
    print("  Box      (orange) – intermediate product (.o / .a)")
    print("  Circle   (green)  – final artifact (executable / .so)")
    print("  Diamond  (color)  – build tool invocation (gcc/g++/as/ar/ld/…)")


if __name__ == "__main__":
    main()

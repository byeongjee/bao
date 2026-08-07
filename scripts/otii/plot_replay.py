#!/usr/bin/env python3
"""Compare a commanded voltage trace against what the Otii device actually produced.

    uv run --extra plot python scripts/otii/plot_replay.py replayed.csv -o replayed.png

Reads the CSV written by replay_trace.py (time_s, commanded_v, measured_v, measured_a)
and, when present, the ``*_commands.csv`` sidecar holding the scheduled and actual send
time of every command. Produces an overlay of commanded and measured voltage, the
tracking error, and the host-side scheduling jitter, and prints the same numbers.
"""

import argparse
import csv
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


def read_csv(path):
    with open(path, newline="") as handle:
        reader = csv.reader(handle)
        header = next(reader)
        columns = {name: [] for name in header}
        for record in reader:
            for name, value in zip(header, record):
                columns[name].append(float(value))
    return {name: np.array(values) for name, values in columns.items()}


def parse_window(text):
    start, end = text.split(",")
    return float(start), float(end)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("replayed", help="CSV written by replay_trace.py")
    parser.add_argument("-o", "--output", required=True, help="output image path")
    parser.add_argument(
        "--zoom",
        default=None,
        metavar="START,END",
        help="seconds to show in the detail panel (default: first 50 ms)",
    )
    args = parser.parse_args()

    data = read_csv(args.replayed)
    t = data["time_s"]
    commanded = data["commanded_v"]
    measured = data["measured_v"]

    commands_path = os.path.splitext(args.replayed)[0] + "_commands.csv"
    commands = read_csv(commands_path) if os.path.exists(commands_path) else None
    jitter = None
    if commands is not None:
        jitter = np.abs(commands["actual_s"] - commands["scheduled_s"]) * 1e6

    inside = t >= 0
    error = measured[inside] - commanded[inside]
    rms = float(np.sqrt(np.mean(error**2)))
    print(f"samples in trace window: {inside.sum()}")
    print(
        f"tracking error: mean {np.mean(np.abs(error)) * 1000:.1f} mV, "
        f"rms {rms * 1000:.1f} mV, max {np.max(np.abs(error)) * 1000:.1f} mV"
    )
    if jitter is not None:
        print(
            f"send jitter: median {np.median(jitter):.1f} us, "
            f"p99 {np.percentile(jitter, 99):.1f} us, max {np.max(jitter):.1f} us"
        )

    if args.zoom:
        zoom_start, zoom_end = parse_window(args.zoom)
    else:
        zoom_start, zoom_end = 0.0, min(0.05, float(t.max()))

    rows = 3 if jitter is not None else 2
    fig, axes = plt.subplots(rows, 1, figsize=(11, 3.0 * rows), constrained_layout=True)

    ax = axes[0]
    ax.plot(t, commanded, lw=1.0, label="commanded", color="#888888")
    ax.plot(t, measured, lw=0.8, label="measured", color="#1f77b4")
    ax.axvspan(zoom_start, zoom_end, color="#ff7f0e", alpha=0.15, label="detail window")
    ax.set_ylabel("voltage (V)")
    ax.set_title(f"{os.path.basename(args.replayed)} — full trace")
    ax.legend(loc="upper right", fontsize=8)
    ax.grid(alpha=0.3)

    ax = axes[1]
    window = (t >= zoom_start) & (t <= zoom_end)
    ax.step(
        t[window],
        commanded[window],
        where="post",
        lw=1.2,
        label="commanded",
        color="#888888",
    )
    ax.plot(
        t[window],
        measured[window],
        lw=1.0,
        marker=".",
        ms=2,
        label="measured",
        color="#1f77b4",
    )
    ax.set_ylabel("voltage (V)")
    ax.set_xlabel("time (s)")
    ax.set_title(
        f"detail {zoom_start:.3f}–{zoom_end:.3f} s  "
        f"(rms error over trace {rms * 1000:.1f} mV)"
    )
    ax.legend(loc="upper right", fontsize=8)
    ax.grid(alpha=0.3)

    if jitter is not None:
        ax = axes[2]
        ax.plot(commands["scheduled_s"], jitter, lw=0.5, color="#d62728")
        ax.set_ylabel("|actual - scheduled| (us)")
        ax.set_xlabel("time (s)")
        ax.set_title("host-side command scheduling jitter")
        ax.set_yscale("log")
        ax.grid(alpha=0.3)

    fig.savefig(args.output, dpi=140)
    print(f"wrote {args.output}")


if __name__ == "__main__":
    sys.exit(main())

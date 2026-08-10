#!/usr/bin/env python3
"""Replay traces on an Otii device and render a PDF comparing each replay
against its source trace.

    uv run --extra otii --extra plot python scripts/otii/check_replayability.py \
        benchmarks/traces/[0-9]*.csv --rate 50 -o replayability.pdf

Any option not listed in ``--help`` is forwarded to replay_trace.py (e.g.
``--rate``, ``--max-current``, ``--send-mode``). Each trace gets one PDF page
showing the commanded and measured voltage plus the instantaneous error, with
summary statistics in the title. The per-trace replay CSVs from replay_trace.py
are written next to the PDF as ``<pdf stem>_<trace stem>.csv``.
"""

import argparse
import csv
import os
import statistics
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import replay_trace
from matplotlib.backends.backend_pdf import PdfPages

MAX_PLOT_POINTS = 40000


def read_replay(path):
    """Read (time_s, commanded_v, measured_v) columns from a replay output CSV."""
    times, commanded, measured = [], [], []
    with open(path, newline="") as handle:
        reader = csv.reader(handle)
        next(reader)
        for row in reader:
            times.append(float(row[0]))
            commanded.append(float(row[1]))
            measured.append(float(row[2]))
    return times, commanded, measured


def error_stats(times, commanded, measured):
    """Mean, RMS and max of |measured - commanded| over the trace proper (t >= 0)."""
    errors = [abs(m - c) for t, c, m in zip(times, commanded, measured) if t >= 0]
    rms = (sum(e * e for e in errors) / len(errors)) ** 0.5
    return statistics.fmean(errors), rms, max(errors)


def add_page(pdf, title, times, commanded, measured):
    stride = max(1, len(times) // MAX_PLOT_POINTS)
    t = times[::stride]
    cmd = commanded[::stride]
    meas = measured[::stride]
    fig, (top, bottom) = plt.subplots(
        2, 1, figsize=(14, 7), sharex=True, height_ratios=[3, 1]
    )
    top.plot(t, cmd, lw=1, label="commanded")
    top.plot(t, meas, lw=1, alpha=0.8, label="measured")
    top.set_ylabel("voltage (V)")
    top.legend(loc="upper right")
    top.set_title(title)
    bottom.plot(t, [(m - c) * 1000 for m, c in zip(meas, cmd)], lw=0.6, color="tab:red")
    bottom.set_ylabel("error (mV)")
    bottom.set_xlabel("time (s)")
    fig.tight_layout()
    pdf.savefig(fig)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "traces", nargs="+", help="input CSVs with time_s,voltage_v columns"
    )
    parser.add_argument("-o", "--output", required=True, help="report PDF path")
    args, replay_options = parser.parse_known_args()

    prefix = os.path.splitext(args.output)[0]
    with PdfPages(args.output) as pdf:
        for trace in args.traces:
            stem = os.path.splitext(os.path.basename(trace))[0]
            replay_csv = f"{prefix}_{stem}.csv"
            replay_args = replay_trace.build_parser().parse_args(
                [trace, "-o", replay_csv] + replay_options
            )
            print(f"\n=== {trace} ===")
            replay_trace.run(replay_args)
            times, commanded, measured = read_replay(replay_csv)
            mean, rms, worst = error_stats(times, commanded, measured)
            add_page(
                pdf,
                f"{trace}: |measured-commanded| mean {mean * 1000:.1f} mV, "
                f"rms {rms * 1000:.1f} mV, max {worst * 1000:.1f} mV",
                times,
                commanded,
                measured,
            )
    print(f"\nwrote {args.output}")


if __name__ == "__main__":
    sys.exit(main())

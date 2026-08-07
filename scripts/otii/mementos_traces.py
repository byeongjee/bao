#!/usr/bin/env python3
"""Fetch the Mementos RFID harvesting traces, convert them to CSV, and plot them.

    uv run --extra plot python scripts/otii/mementos_traces.py --dir tmp/mementos-traces

Source: https://github.com/ransford/mspsim/tree/mementos/traces — ten 1 kHz recordings
of a WISP 4.1 analog front-end loaded with 30 kohm, taken while walking near an RFID
reader.

The upstream README describes the first column as a millisecond counter, but it is
actually ``minutes * 100000 + seconds * 1000 + milliseconds``: the low five digits run
00000..59999 and then the minute prefix increments. Decoding it as plain milliseconds
inserts a phantom 40 s gap at every minute boundary. Since the decoded grid is uniform
1 ms apart from a handful of corrupted samples, the exported time base is simply
``sample_index / 1000``; the timestamp column is only used to verify that.

Writes ``<dir>/csv/<n>.csv`` with time_s,voltage_v columns, ready for replay_trace.py.
"""

import argparse
import csv
import os
import sys
import urllib.request

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

BASE_URL = "https://raw.githubusercontent.com/ransford/mspsim/mementos/traces"
TRACES = [str(n) for n in range(1, 11)]
SAMPLE_RATE = 1000.0
# Rising-edge slew rate measured on the Otii Ace over 1.3 V and 3.6 V steps.
OTII_SLEW_V_PER_S = 480.0


def fetch(directory, name):
    path = os.path.join(directory, f"{name}.txt")
    if not os.path.exists(path):
        urllib.request.urlretrieve(f"{BASE_URL}/{name}.txt", path)
    return path


def decode_time_ms(raw):
    """Undo the minutes/seconds/milliseconds packing of the timestamp column."""
    minutes = raw // 100000
    remainder = raw % 100000
    return (minutes * 60 + remainder // 1000) * 1000 + remainder % 1000


def load(path):
    """Return (voltages, corrupted_sample_count) and assert a uniform 1 ms grid."""
    data = np.loadtxt(path)
    raw = data[:, 0].astype(np.int64)
    voltage = data[:, 1]
    step = np.diff(decode_time_ms(raw))
    corrupted = int(np.count_nonzero(step != 1))
    gaps = step[(step != 1) & (step != 1001) & (step != -999)]
    if gaps.size:
        raise SystemExit(f"{path}: unexpected timestamp steps {np.unique(gaps)}")
    return voltage, corrupted


def write_csv(path, voltage):
    with open(path, "w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["time_s", "voltage_v"])
        for index, value in enumerate(voltage):
            writer.writerow([f"{index / SAMPLE_RATE:.3f}", f"{value:.6f}"])


def hold_resample(voltage, rate):
    """Zero-order hold the 1 kHz trace at `rate` Hz, back on the original grid."""
    factor = round(SAMPLE_RATE / rate)
    return voltage[np.arange(len(voltage)) // factor * factor]


def plot_grid(traces, path, low, high):
    fig, axes = plt.subplots(5, 2, figsize=(14, 15), constrained_layout=True)
    for ax, (name, voltage) in zip(axes.ravel(), traces):
        t = np.arange(len(voltage)) / SAMPLE_RATE
        ax.plot(t, voltage, lw=0.3, color="#1f77b4")
        ax.axhspan(low, high, color="#2ca02c", alpha=0.12)
        ax.set_title(
            f"trace {name} — {len(voltage) / SAMPLE_RATE:.1f} s, "
            f"{voltage.min():.2f}–{voltage.max():.2f} V, mean {voltage.mean():.2f} V",
            fontsize=10,
        )
        ax.set_xlabel("time (s)")
        ax.set_ylabel("voltage (V)")
        ax.grid(alpha=0.3)
    fig.suptitle(
        "Mementos RFID harvesting traces (WISP 4.1 front-end, 30 kohm load, 1 kHz)\n"
        f"green band = MSP430 operating range {low}–{high} V",
        fontsize=13,
    )
    fig.savefig(path, dpi=110)
    plt.close(fig)


def plot_replayability(traces, path):
    fig, axes = plt.subplots(1, 2, figsize=(14, 5.5), constrained_layout=True)
    colors = plt.cm.viridis(np.linspace(0, 0.9, len(traces)))

    ax = axes[0]
    for (name, voltage), color in zip(traces, colors):
        slew = np.abs(np.diff(voltage)) * SAMPLE_RATE
        ordered = np.sort(slew)
        ax.plot(
            ordered,
            np.linspace(0, 100, len(ordered)),
            lw=1.2,
            color=color,
            label=f"trace {name}",
        )
    ax.axvline(OTII_SLEW_V_PER_S, color="#d62728", ls="--", lw=1.5)
    ax.text(
        OTII_SLEW_V_PER_S * 1.1,
        20,
        f"Otii rise limit\n{OTII_SLEW_V_PER_S:.0f} V/s",
        color="#d62728",
        fontsize=9,
    )
    ax.set_xscale("log")
    ax.set_xlabel("|dV/dt| between consecutive 1 kHz samples (V/s)")
    ax.set_ylabel("percentile (%)")
    ax.set_title("How steep are the traces?")
    ax.legend(fontsize=7, ncol=2, loc="lower right")
    ax.grid(alpha=0.3)

    ax = axes[1]
    rates = [500, 200, 100, 50, 20, 10]
    for (name, voltage), color in zip(traces, colors):
        errors = []
        for rate in rates:
            residual = voltage - hold_resample(voltage, rate)
            errors.append(np.sqrt(np.mean(residual**2)) * 1000)
        ax.plot(
            rates, errors, marker="o", ms=4, lw=1.2, color=color, label=f"trace {name}"
        )
    ax.axvline(100, color="#d62728", ls=":", lw=1.2)
    ax.axvline(50, color="#d62728", ls="--", lw=1.5)
    ax.text(52, ax.get_ylim()[1] * 0.9, "Otii clean rate", color="#d62728", fontsize=9)
    ax.set_xscale("log")
    ax.set_xlabel("replay rate (Hz)")
    ax.set_ylabel("rms error vs 1 kHz original (mV)")
    ax.set_title("What a lower replay rate costs")
    ax.legend(fontsize=7, ncol=2, loc="upper right")
    ax.grid(alpha=0.3)

    fig.suptitle("Replaying the Mementos traces on the Otii Ace", fontsize=13)
    fig.savefig(path, dpi=110)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--dir", default="tmp/mementos-traces", help="download/output directory"
    )
    parser.add_argument(
        "--vmin", type=float, default=1.8, help="target operating minimum (V)"
    )
    parser.add_argument(
        "--vmax", type=float, default=3.6, help="target operating maximum (V)"
    )
    args = parser.parse_args()

    csv_dir = os.path.join(args.dir, "csv")
    os.makedirs(csv_dir, exist_ok=True)

    traces = []
    print(
        f"{'trace':>6} {'samples':>8} {'seconds':>8} {'min V':>7} {'max V':>7} "
        f"{'mean V':>7} {'in band %':>10} {'bad ts':>7}"
    )
    for name in TRACES:
        voltage, corrupted = load(fetch(args.dir, name))
        write_csv(os.path.join(csv_dir, f"{name}.csv"), voltage)
        in_band = np.mean((voltage >= args.vmin) & (voltage <= args.vmax)) * 100
        print(
            f"{name:>6} {len(voltage):>8} {len(voltage) / SAMPLE_RATE:>8.1f} "
            f"{voltage.min():>7.3f} {voltage.max():>7.3f} {voltage.mean():>7.3f} "
            f"{in_band:>10.1f} {corrupted:>7}"
        )
        traces.append((name, voltage))

    grid_path = os.path.join(args.dir, "traces_grid.png")
    replay_path = os.path.join(args.dir, "traces_replayability.png")
    plot_grid(traces, grid_path, args.vmin, args.vmax)
    plot_replayability(traces, replay_path)

    steep = [
        (name, np.mean(np.abs(np.diff(v)) * SAMPLE_RATE > OTII_SLEW_V_PER_S) * 100)
        for name, v in traces
    ]
    worst = max(steep, key=lambda row: row[1])
    print(
        f"\nsteps steeper than the Otii's {OTII_SLEW_V_PER_S:.0f} V/s rise limit: "
        f"worst is trace {worst[0]} at {worst[1]:.2f}% of samples"
    )
    print(f"wrote {csv_dir}/*.csv, {grid_path}, {replay_path}")


if __name__ == "__main__":
    sys.exit(main())

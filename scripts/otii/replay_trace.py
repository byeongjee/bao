#!/usr/bin/env python3
"""Replay a voltage trace on an Otii device and record the voltage it actually produced.

    uv run --extra otii python scripts/otii/replay_trace.py trace.csv -o replayed.csv

Input CSV holds two columns, time in seconds and voltage in volts; a header row is
optional. The trace is resampled with a zero-order hold onto a uniform grid at
``--rate`` Hz, and each grid point is pushed to the device with ``arc_set_main_voltage``
on an absolute-deadline schedule.

An alignment marker (hold, short pulse, hold) is prepended to the schedule so the
recorded trace can be shifted onto the command time base by its first rising edge.
Constant end-to-end latency is therefore removed by construction; what remains in the
output is waveform fidelity.

Output CSV columns: time_s, commanded_v, measured_v, measured_a. A sidecar
``*_commands.csv`` records the scheduled and actual send time of every command so
host-side scheduling jitter can be separated from the device response.
"""

import argparse
import csv
import json
import os
import socket
import statistics
import subprocess
import sys
import threading
import time

from otii_tcp_client import otii_client, otii_connection

CLOCK = time.perf_counter_ns
MARKER_PULSE_S = 0.1
DRAIN_QUIET_S = 0.5


# --------------------------------------------------------------------------- trace


def load_trace(path):
    """Read (time_s, voltage_v) pairs, skipping any non-numeric header rows."""
    rows = []
    with open(path, newline="") as handle:
        for record in csv.reader(handle):
            if len(record) < 2:
                continue
            try:
                rows.append((float(record[0]), float(record[1])))
            except ValueError:
                continue
    if len(rows) < 2:
        raise SystemExit(f"{path}: need at least two numeric (time, voltage) rows")
    return rows


def resample(rows, rate):
    """Zero-order hold the trace onto a uniform grid at `rate` Hz."""
    t0 = rows[0][0]
    duration = rows[-1][0] - t0
    count = round(duration * rate) + 1
    levels = []
    index = 0
    for step in range(count):
        t = step / rate
        while index + 1 < len(rows) and rows[index + 1][0] - t0 <= t:
            index += 1
        levels.append(rows[index][1])
    return levels


# --------------------------------------------------------------------------- server


def wait_for_port(host, port, timeout):
    end = CLOCK() + int(timeout * 1e9)
    while CLOCK() < end:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def start_server(host, port):
    """Start otii_server unless one is already listening. Returns the Popen or None."""
    if wait_for_port(host, port, 0.2):
        return None
    binary = os.environ.get("OTII_SERVER_BIN", "otii_server")
    proc = subprocess.Popen(
        [binary], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    if not wait_for_port(host, port, 20.0):
        proc.terminate()
        raise SystemExit(
            f"otii_server ({binary}) did not start listening on {host}:{port}"
        )
    return proc


# --------------------------------------------------------------------------- sending


class VoltageSender:
    """Pushes arc_set_main_voltage over the client's own socket.

    In async mode the requests are written back to back without waiting for a
    response, so the per-command cost is a single write instead of a round trip. A
    background thread keeps draining the response stream so the socket buffer cannot
    fill up and stall the sender.
    """

    def __init__(self, connection, device_id):
        self._connection = connection
        self._device_id = device_id
        self._sock = connection.sock
        self._sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._stop = threading.Event()
        self._thread = None
        self._last_recv = CLOCK()
        self.errors = 0
        self.error_samples = []

    def encode(self, voltage):
        request = {
            "type": "request",
            "cmd": "arc_set_main_voltage",
            "trans_id": otii_connection.get_new_trans_id(),
            "data": {"device_id": self._device_id, "value": voltage},
        }
        return (json.dumps(request) + "\r\n").encode("utf-8")

    def send(self, payload):
        self._sock.sendall(payload)

    def send_sync(self, voltage):
        self._connection.send_and_receive(
            {
                "type": "request",
                "cmd": "arc_set_main_voltage",
                "data": {"device_id": self._device_id, "value": voltage},
            }
        )

    def start_draining(self):
        self._sock.settimeout(0.1)
        self._thread = threading.Thread(target=self._drain, daemon=True)
        self._thread.start()

    def _drain(self):
        while not self._stop.is_set():
            try:
                chunk = self._sock.recv(256 * 1024)
            except TimeoutError:
                continue
            except OSError:
                return
            if not chunk:
                return
            self._last_recv = CLOCK()
            self.errors += chunk.count(b'"type":"error"')
            if self.errors and len(self.error_samples) < 3:
                for line in chunk.split(b"\r\n"):
                    if b'"type":"error"' in line and len(self.error_samples) < 3:
                        self.error_samples.append(line.decode("utf-8", "replace"))

    def stop_draining(self):
        """Wait until the response stream goes quiet, then hand the socket back."""
        quiet = int(DRAIN_QUIET_S * 1e9)
        while CLOCK() - self._last_recv < quiet:
            time.sleep(0.05)
        self._stop.set()
        self._thread.join(timeout=2.0)
        self._sock.settimeout(None)
        self._connection.recv_msg = ""


def sleep_until(deadline_ns, spin_ns):
    """Block until `deadline_ns`, burning the last `spin_ns` in a busy loop.

    CPython has no clock_nanosleep binding, so a coarse sleep is followed by a spin;
    on macOS there is no SCHED_FIFO equivalent either, and the spin is what keeps the
    tail of the jitter distribution bounded. Long sleeps get coalesced by the kernel and
    overshoot by a millisecond or more, so the gap is halved repeatedly rather than
    slept away in one call.
    """
    while True:
        remaining = deadline_ns - CLOCK()
        if remaining <= spin_ns:
            break
        time.sleep(remaining / 2e9)
    while CLOCK() < deadline_ns:
        pass


def replay(sender, schedule, rate, delta_threshold, spin_ns, mode):
    """Drive the schedule and return one (scheduled_s, actual_s, voltage, sent) row per step."""
    period_ns = round(1e9 / rate)
    payloads = None
    if mode == "async":
        payloads = [sender.encode(v) for v in schedule]

    log = []
    last_sent = None
    origin = CLOCK() + period_ns
    for step, voltage in enumerate(schedule):
        deadline = origin + step * period_ns
        sleep_until(deadline, spin_ns)
        skip = last_sent is not None and abs(voltage - last_sent) < delta_threshold
        actual = CLOCK()
        if not skip:
            if mode == "async":
                sender.send(payloads[step])
            else:
                sender.send_sync(voltage)
            last_sent = voltage
        log.append(
            ((deadline - origin) / 1e9, (actual - origin) / 1e9, voltage, not skip)
        )
    return log


# --------------------------------------------------------------------------- readback


def read_channel(recording, device_id, channel):
    """Fetch a whole analog channel as (t0, interval, values)."""
    count = recording.get_channel_data_count(device_id, channel)
    if count == 0:
        raise SystemExit(f"no samples recorded on channel '{channel}'")
    values = []
    t0 = None
    interval = None
    index = 0
    while index < count:
        chunk = recording.get_channel_data(
            device_id, channel, index, min(40000, count - index)
        )
        if t0 is None:
            t0 = chunk["timestamp"]
            interval = chunk["interval"]
        values.extend(chunk["values"])
        index += len(chunk["values"])
    return t0, interval, values


def find_rising_edge(values, t0, interval, threshold):
    """Time of the first sample at or above `threshold`."""
    for index, value in enumerate(values):
        if value >= threshold:
            return t0 + index * interval
    return None


# --------------------------------------------------------------------------- output


def write_measured(path, times, commanded, measured_v, measured_a):
    with open(path, "w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["time_s", "commanded_v", "measured_v", "measured_a"])
        for row in zip(times, commanded, measured_v, measured_a):
            writer.writerow(
                [f"{row[0]:.6f}", f"{row[1]:.6f}", f"{row[2]:.6f}", f"{row[3]:.9f}"]
            )


def write_commands(path, log, trace_start_s):
    with open(path, "w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["scheduled_s", "actual_s", "voltage_v", "sent"])
        for scheduled, actual, voltage, sent in log:
            writer.writerow(
                [
                    f"{scheduled - trace_start_s:.6f}",
                    f"{actual - trace_start_s:.6f}",
                    f"{voltage:.6f}",
                    int(sent),
                ]
            )


# --------------------------------------------------------------------------- main


def build_parser():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("trace", help="input CSV with time_s,voltage_v columns")
    parser.add_argument("-o", "--output", required=True, help="output CSV path")
    parser.add_argument("--rate", type=float, default=1000.0, help="replay rate in Hz")
    parser.add_argument(
        "--max-current", type=float, default=0.05, help="current limit in A"
    )
    parser.add_argument(
        "--delta-threshold",
        type=float,
        default=0.0,
        help="skip a command when the level moved less than this many volts",
    )
    parser.add_argument(
        "--spin-us",
        type=float,
        default=300.0,
        help="busy-wait window at the end of each period",
    )
    parser.add_argument(
        "--send-mode",
        choices=("async", "sync"),
        default="async",
        help="async pipelines requests; sync waits for each response",
    )
    parser.add_argument(
        "--marker-hold",
        type=float,
        default=0.3,
        help="seconds held at the low level before the alignment pulse",
    )
    parser.add_argument(
        "--settle",
        type=float,
        default=1.0,
        help="seconds to settle after enabling main power",
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=1905)
    return parser


def run(args):
    rows = load_trace(args.trace)
    levels = resample(rows, args.rate)
    lo = min(levels)
    hi = max(levels)
    if hi - lo < 0.1:
        raise SystemExit(
            "trace spans less than 0.1 V; the alignment marker needs a real edge"
        )

    hold_n = round(args.marker_hold * args.rate)
    pulse_n = round(MARKER_PULSE_S * args.rate)
    schedule = [lo] * hold_n + [hi] * pulse_n + [lo] * pulse_n + levels
    edge_s = hold_n / args.rate
    trace_start_s = (hold_n + 2 * pulse_n) / args.rate
    threshold = (lo + hi) / 2.0

    print(
        f"trace: {len(rows)} rows -> {len(levels)} samples at {args.rate:g} Hz "
        f"({len(levels) / args.rate:.3f} s), {lo:.3f}..{hi:.3f} V"
    )
    print(f"schedule: {len(schedule)} commands, mode={args.send_mode}")

    server = start_server(args.host, args.port)
    otii = otii_client.OtiiClient().connect(host=args.host, port=args.port)
    try:
        devices = otii.get_devices()
        if not devices:
            raise SystemExit("no Otii device found")
        arc = devices[0]
        project = otii.create_project()
        arc.add_to_project()
        print(f"device: {arc.name} (id {arc.id}) fw {arc.get_version()['fw_version']}")

        arc.enable_channel("mc", True)
        arc.enable_channel("mv", True)
        arc.set_max_current(args.max_current)
        arc.set_main_voltage(lo)
        arc.set_main(True)
        time.sleep(args.settle)

        project.start_recording()
        time.sleep(0.2)

        sender = VoltageSender(otii.connection, arc.id)
        if args.send_mode == "async":
            sender.start_draining()
        log = replay(
            sender,
            schedule,
            args.rate,
            args.delta_threshold,
            int(args.spin_us * 1000),
            args.send_mode,
        )
        if args.send_mode == "async":
            sender.stop_draining()

        time.sleep(0.2)
        project.stop_recording()
        arc.set_main(False)

        recording = project.get_last_recording()
        mv_t0, mv_dt, mv = read_channel(recording, arc.id, "mv")
        mc_t0, mc_dt, mc = read_channel(recording, arc.id, "mc")
    finally:
        otii.disconnect()
        if server:
            server.terminate()

    edge_t = find_rising_edge(mv, mv_t0, mv_dt, threshold)
    if edge_t is None:
        raise SystemExit(
            f"alignment marker not found: no recorded sample reached {threshold:.3f} V"
        )
    offset = edge_t - edge_s

    times = []
    commanded = []
    measured_v = []
    measured_a = []
    duration = len(levels) / args.rate
    for index, value in enumerate(mv):
        t = (mv_t0 + index * mv_dt) - offset - trace_start_s
        if t < -0.05 or t > duration + 0.05:
            continue
        step = min(len(levels) - 1, max(0, int(t * args.rate)))
        current_index = round(((t + trace_start_s + offset) - mc_t0) / mc_dt)
        times.append(t)
        commanded.append(levels[step] if t >= 0 else lo)
        measured_v.append(value)
        measured_a.append(
            mc[current_index] if 0 <= current_index < len(mc) else float("nan")
        )

    write_measured(args.output, times, commanded, measured_v, measured_a)
    commands_path = os.path.splitext(args.output)[0] + "_commands.csv"
    write_commands(commands_path, log, trace_start_s)

    lateness = [abs(actual - scheduled) for scheduled, actual, _, _ in log]
    lateness.sort()
    period = 1.0 / args.rate
    sent = sum(1 for row in log if row[3])
    print(
        f"\nrecorded: mv {len(mv)} samples at {1 / mv_dt:.0f} Hz, "
        f"mc {len(mc)} samples at {1 / mc_dt:.0f} Hz"
    )
    print(f"marker edge at t={edge_t:.6f} s -> offset {offset:.6f} s")
    print(f"commands sent: {sent}/{len(schedule)}, error responses: {sender.errors}")
    for sample in sender.error_samples:
        print(f"  error: {sample}")
    print(
        "send jitter |actual-scheduled|: "
        f"median {lateness[len(lateness) // 2] * 1e6:.1f} us, "
        f"p99 {lateness[int(len(lateness) * 0.99)] * 1e6:.1f} us, "
        f"max {lateness[-1] * 1e6:.1f} us"
    )
    print(
        f"periods overrun (>{period * 1e6:.0f} us late): "
        f"{sum(1 for value in lateness if value > period)}/{len(lateness)}"
    )

    errors = [abs(m - c) for t, c, m in zip(times, commanded, measured_v) if t >= 0]
    if errors:
        rms = (sum(value * value for value in errors) / len(errors)) ** 0.5
        print(
            f"|measured-commanded| over the trace: mean {statistics.fmean(errors) * 1000:.1f} mV, "
            f"rms {rms * 1000:.1f} mV, max {max(errors) * 1000:.1f} mV"
        )
    print(f"\nwrote {args.output} ({len(times)} rows) and {commands_path}")


def main():
    run(build_parser().parse_args())


if __name__ == "__main__":
    sys.exit(main())

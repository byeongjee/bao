# Saleae Logic Setup for Execution Timing

`ckpt bench` and `ckpt verify` measure on-device execution time by capturing
GPIO pulses from the MSP430 board with a Saleae Logic analyzer. This document
is the single source of truth for the hardware wiring and software setup.

## Hardware

- A Saleae Logic analyzer supported by the Logic 2 app.
- Wire MSP430FR5994 **P3.4** to Saleae **digital channel 0**, and connect
  ground between the board and the analyzer.
- The channel is sampled at 100 MHz.

## Software

1. Install the [Saleae Logic 2](https://www.saleae.com/downloads/) desktop app.
2. Enable the automation server in Logic 2 (Preferences → Automation). The
   toolchain connects to it at `localhost:10430`.
3. Install the Python bindings: `uv sync --extra saleae`.
4. Keep Logic 2 running (with automation enabled) while `ckpt bench` or
   `ckpt verify` executes.

## How the Measurement Works

The benchmark runtime (`passes/runtime/benchmark.h`) signals with positive
pulses on P3.4:

| Event | Pulse width | Role |
|-------|-------------|------|
| Start | ~10 µs | Marks execution start; below the trigger threshold |
| Stop  | ~5 ms  | Marks execution end; fires the capture trigger |

The capture triggers on `PULSE_HIGH` with a 1 ms minimum pulse width, so only
the stop pulse ends the capture. Execution time is extracted in
post-processing as the delta from the start-pulse falling edge to the
stop-pulse rising edge.

The GPIO stays LOW during execution, so brown-out resets — which reset port
registers to LOW — are invisible to the capture (pulse-based, BOR-safe
signalling).

Per measurement, the runner arms a capture, flashes the ELF via mspdebug,
lets the target free-run, waits for the stop-pulse trigger, and exports the
digital data to compute the delta. Ambiguous captures are retried up to
3 times.

## Caveats

- **Always use the built-in `--timeout` option** of `ckpt bench` / `ckpt
  verify`. Never wrap them with `timeout`, `gtimeout`, or another external
  process killer: killing the Python process externally bypasses capture
  cleanup and can leave Logic 2 unable to start a new capture session
  (restart Logic 2 to recover).

## Troubleshooting

- `Cannot connect to Saleae Logic 2 automation server (localhost:10430)` —
  Logic 2 is not running, or the automation server is not enabled.
- `No edges detected` — the GPIO never pulsed; check the P3.4 wiring and
  that the board is powered and flashed.

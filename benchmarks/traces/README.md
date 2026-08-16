# Harvesting Traces

`original/` holds the ten Mementos RFID harvesting traces
([mspsim `mementos` branch](https://github.com/ransford/mspsim/tree/mementos/traces)):
1 kHz voltage recordings of a WISP 4.1 analog front-end loaded with 30 kΩ,
taken while walking near an RFID reader.

The `*.csv` files here are the replay-ready versions: block-averaged from
1 kHz to 50 Hz (each sample is the mean of a 20 ms window, which acts as the
anti-aliasing filter), clipped to 3.6 V, the MSP430 operating maximum, and
written as 8 back-to-back repetitions. Reproduce with:

```bash
uv run python scripts/otii/preprocess_traces.py benchmarks/traces/original/[0-9]*.txt \
    -o benchmarks/traces --vmax 3.6 --repeat 8
```

Trace 3 peaks at 3.13 V, below the target's operating range, so it is scaled up
first (`SCALE_TO_LEVEL` in the script), anchored on its 99th percentile.

`traces_grid.png` overlays the 50 Hz traces on the 1 kHz originals.

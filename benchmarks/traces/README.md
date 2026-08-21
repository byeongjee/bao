# Harvesting Traces

`original/` holds the ten Mementos RFID harvesting traces
([mspsim `mementos` branch](https://github.com/ransford/mspsim/tree/mementos/traces)):
1 kHz voltage recordings of a WISP 4.1 analog front-end loaded with 30 kΩ,
taken while walking near an RFID reader.

The `*.csv` files here are the replay-ready versions: time-compressed 10x
(the walks have a power failure only every 5-50 s, far slower than the
benchmarks run; at 10x the replays fail 0.2-2/s), block-averaged to 50 Hz
(each sample is the mean of a 200 ms window of the recording, which acts as
the anti-aliasing filter), clipped to 3.6 V, the MSP430 operating maximum,
and written as 80 back-to-back repetitions. Reproduce with:

```bash
uv run python scripts/otii/preprocess_traces.py benchmarks/traces/original/[0-9]*.txt \
    -o benchmarks/traces --vmax 3.6 --speedup 10 --repeat 80
```

Trace 3 peaks at 3.13 V, below the target's operating range, so it is scaled up
first (`SCALE_TO_LEVEL` in the script), anchored on its 99th percentile.

`traces_grid.pdf` shows one repetition period of each replay-ready trace
(`Rscript scripts/plot_power_traces.R benchmarks/traces`).

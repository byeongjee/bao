#!/bin/bash
# Continuous-power reference for the decomposition (summarize.py): the same
# wait-mode binaries at the board capacitance, run at a constant 3.3 V so no
# boundary ever waits, plus the uninstrumented O3 baseline. Both are built
# with INTERMITTENT_BUILD, the define `ckpt intermittent` uses (it shrinks
# dijkstra, qsort, and bitcount). Wire the Otii main output directly to the
# board rail (no 560 Ohm resistor, no diode) as in docs/intermittent.md.
cd "$(dirname "$0")/../.." || exit 1
BENCHMARKS="${BENCHMARKS:-aes crc rsa dijkstra qsort activity_recognition bitcount chacha20 sensor_fusion poly1305 cuckoo_filter sha256 stringsearch}"
OUT=results/intermittent/continuous
mkdir -p "$OUT"
for a in milp rockclimb schematic schematicO3; do
  echo "=== $(date '+%F %T') start $a"
  uv run ckpt bench "$a" $BENCHMARKS --cap board --halt-mode wait -D INTERMITTENT_BUILD --csv "$OUT/$a.csv" > "$OUT/$a.log" 2>&1
  echo "=== $(date '+%F %T') end   $a exit=$?"
done
echo "=== $(date '+%F %T') start uninstrumented"
uv run ckpt bench uninstrumented $BENCHMARKS -D INTERMITTENT_BUILD --csv "$OUT/uninstrumented.csv" > "$OUT/uninstrumented.log" 2>&1
echo "=== $(date '+%F %T') end   uninstrumented exit=$?"

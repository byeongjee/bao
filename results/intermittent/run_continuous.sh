#!/bin/bash
# Continuous-power reference for the decomposition (summarize.py): the same
# wait-mode binaries at the board capacitance, run at a constant 3.3 V so no
# boundary ever waits. Wire the Otii main output directly to the board rail
# (no 560 Ohm resistor, no diode) as in docs/intermittent.md. The
# uninstrumented reference is results/uninstrumented.csv.
cd "$(dirname "$0")/../.." || exit 1
BENCHMARKS="aes crc rsa dijkstra qsort activity_recognition bitcount chacha20 sensor_fusion poly1305 cuckoo_filter sha256 stringsearch"
OUT=results/intermittent/continuous
mkdir -p "$OUT"
for a in milp rockclimb schematic schematicO3; do
  echo "=== $(date '+%F %T') start $a"
  uv run ckpt bench "$a" $BENCHMARKS --cap board --halt-mode wait --csv "$OUT/$a.csv" > "$OUT/$a.log" 2>&1
  echo "=== $(date '+%F %T') end   $a exit=$?"
done

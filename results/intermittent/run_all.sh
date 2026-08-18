#!/bin/bash
# Runs `ckpt intermittent <algo> <benchmark>` one benchmark at a time over all
# traces. Resumable: a (benchmark, algo) CSV with one row per trace is skipped;
# a partial one (interrupted run) is deleted and redone.
cd "$(dirname "$0")/../.." || exit 1
BENCHMARKS="aes crc rsa dijkstra qsort activity_recognition bitcount chacha20 sensor_fusion poly1305 cuckoo_filter sha256_fixed stringsearch_fixed"
ALGOS="milp rockclimb schematic schematicO3"
TRACES=1,2,3,4,5,6,7,8,9,10
NTRACES=10
OUT=results/intermittent
for b in $BENCHMARKS; do
  for a in $ALGOS; do
    csv=$OUT/$b/$a.csv
    if [ -f "$csv" ] && [ "$(tail -n +2 "$csv" | grep -c .)" -eq $NTRACES ]; then
      echo "skip $b $a (complete)"; continue
    fi
    rm -f "$csv"
    mkdir -p "$OUT/$b"
    echo "=== $(date '+%F %T') start $b $a"
    uv run ckpt intermittent "$a" "$b" --trace $TRACES --csv "$csv" > "$OUT/$b/$a.log" 2>&1
    echo "=== $(date '+%F %T') end   $b $a exit=$?"
  done
done
touch "$OUT/DONE"

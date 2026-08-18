# Intermittent-power results

13 benchmarks x 4 algorithms x 10 traces (benchmarks/traces/1..10.csv), board cap (11.5 uF), halt-mode wait, cpu 16 MHz, no device-debug.

Cell: `ok` = completed (stop pulse + __nvm_done) and return code equals the uninstrumented baseline (`WRONG` otherwise); `incomplete` = did not finish within the trace; rec = recovery boots (cnt_recovery); t = execution time incl. wait/outage time (Saleae start->stop pulse).

## Totals per algorithm

| algo | runs | complete | correct (of complete) | incomplete/other | mean rec (complete) | mean t (complete, s) |
|---|---|---|---|---|---|---|
| milp | 130 | 130 | 130 | 0 | 0.2 | 1.36 |
| rockclimb | 130 | 105 | 105 | 25 | 7.4 | 41.37 |
| schematic | 130 | 130 | 130 | 0 | 1.6 | 9.71 |
| schematicO3 | 130 | 130 | 130 | 0 | 0.4 | 2.20 |

## Per benchmark: complete / correct / total rec / mean t (s)

| benchmark (baseline result) | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes (107) | 10/10 / 10/10 / 2 / 1.23 | 10/10 / 10/10 / 45 / 27.87 | 10/10 / 10/10 / 85 / 52.04 | 10/10 / 10/10 / 2 / 1.25 |
| crc (39423) | 10/10 / 10/10 / 2 / 0.76 | 10/10 / 10/10 / 33 / 21.39 | 10/10 / 10/10 / 18 / 10.61 | 10/10 / 10/10 / 2 / 0.77 |
| rsa (32) | 10/10 / 10/10 / 7 / 4.20 | 3/10 / 3/3 / 80 / 142.64 | 10/10 / 10/10 / 18 / 10.86 | 10/10 / 10/10 / 12 / 6.19 |
| dijkstra (3788) | 10/10 / 10/10 / 2 / 1.26 | 3/10 / 3/3 / 102 / 176.37 | 10/10 / 10/10 / 12 / 6.31 | 10/10 / 10/10 / 2 / 1.34 |
| qsort (9987) | 10/10 / 10/10 / 2 / 1.36 | 10/10 / 10/10 / 152 / 89.84 | 10/10 / 10/10 / 7 / 3.93 | 10/10 / 10/10 / 6 / 3.10 |
| activity_recognition (64) | 10/10 / 10/10 / 2 / 1.35 | 10/10 / 10/10 / 50 / 31.56 | 10/10 / 10/10 / 27 / 18.70 | 10/10 / 10/10 / 6 / 3.08 |
| bitcount (14121) | 10/10 / 10/10 / 2 / 1.26 | 6/10 / 6/6 / 184 / 163.98 | 10/10 / 10/10 / 17 / 8.94 | 10/10 / 10/10 / 2 / 1.34 |
| chacha20 (97) | 10/10 / 10/10 / 0 / 0.00 | 10/10 / 10/10 / 2 / 0.77 | 10/10 / 10/10 / 2 / 0.73 | 10/10 / 10/10 / 0 / 0.00 |
| sensor_fusion (613) | 10/10 / 10/10 / 2 / 1.37 | 10/10 / 10/10 / 2 / 1.36 | 10/10 / 10/10 / 2 / 1.35 | 10/10 / 10/10 / 7 / 3.88 |
| poly1305 (108) | 10/10 / 10/10 / 0 / 0.00 | 10/10 / 10/10 / 0 / 0.00 | 10/10 / 10/10 / 0 / 0.00 | 10/10 / 10/10 / 0 / 0.00 |
| cuckoo_filter (256) | 10/10 / 10/10 / 0 / 0.00 | 10/10 / 10/10 / 6 / 3.15 | 10/10 / 10/10 / 2 / 1.19 | 10/10 / 10/10 / 2 / 0.73 |
| sha256_fixed (244) | 10/10 / 10/10 / 2 / 0.74 | 10/10 / 10/10 / 6 / 3.09 | 10/10 / 10/10 / 2 / 0.88 | 10/10 / 10/10 / 2 / 0.73 |
| stringsearch_fixed (20) | 10/10 / 10/10 / 7 / 4.19 | 3/10 / 3/3 / 118 / 204.28 | 10/10 / 10/10 / 18 / 10.65 | 10/10 / 10/10 / 12 / 6.19 |

## Per benchmark x algorithm x trace

### aes (baseline result 107)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok rec=0 t=0.15s | ok rec=0 t=2.48s | ok rec=0 t=0.14s | ok rec=0 t=1.41s | ok rec=0 t=0.21s | ok rec=0 t=0.23s | ok rec=0 t=0.14s | ok rec=0 t=0.15s | ok rec=2 t=7.19s | ok rec=0 t=0.15s |
| rockclimb | ok rec=0 t=1.70s | ok rec=0 t=7.33s | ok rec=3 t=41.82s | ok rec=10 t=54.45s | ok rec=9 t=49.68s | ok rec=1 t=11.30s | ok rec=8 t=30.63s | ok rec=7 t=47.16s | ok rec=7 t=32.90s | ok rec=0 t=1.70s |
| schematic | ok rec=1 t=8.99s | ok rec=1 t=16.40s | ok rec=6 t=79.36s | ok rec=20 t=106.75s | ok rec=14 t=80.64s | ok rec=1 t=12.35s | ok rec=15 t=55.23s | ok rec=8 t=62.20s | ok rec=12 t=58.13s | ok rec=7 t=40.37s |
| schematicO3 | ok rec=0 t=0.19s | ok rec=0 t=2.51s | ok rec=0 t=0.15s | ok rec=0 t=1.48s | ok rec=0 t=0.24s | ok rec=0 t=0.29s | ok rec=0 t=0.15s | ok rec=0 t=0.16s | ok rec=2 t=7.18s | ok rec=0 t=0.15s |

### crc (baseline result 39423)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok rec=0 t=0.06s | ok rec=0 t=0.06s | ok rec=0 t=0.03s | ok rec=0 t=0.06s | ok rec=0 t=0.06s | ok rec=0 t=0.06s | ok rec=0 t=0.04s | ok rec=0 t=0.06s | ok rec=2 t=7.09s | ok rec=0 t=0.06s |
| rockclimb | ok rec=0 t=1.25s | ok rec=0 t=6.48s | ok rec=3 t=39.61s | ok rec=10 t=53.94s | ok rec=5 t=31.39s | ok rec=1 t=10.83s | ok rec=6 t=20.71s | ok rec=1 t=16.02s | ok rec=7 t=32.44s | ok rec=0 t=1.24s |
| schematic | ok rec=0 t=0.85s | ok rec=0 t=5.61s | ok rec=0 t=12.49s | ok rec=5 t=27.76s | ok rec=4 t=18.62s | ok rec=1 t=10.31s | ok rec=6 t=20.26s | ok rec=0 t=1.49s | ok rec=2 t=7.87s | ok rec=0 t=0.86s |
| schematicO3 | ok rec=0 t=0.07s | ok rec=0 t=0.07s | ok rec=0 t=0.06s | ok rec=0 t=0.07s | ok rec=0 t=0.09s | ok rec=0 t=0.07s | ok rec=0 t=0.07s | ok rec=0 t=0.07s | ok rec=2 t=7.10s | ok rec=0 t=0.07s |

### rsa (baseline result 32)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok rec=0 t=0.52s | ok rec=0 t=4.21s | ok rec=0 t=2.21s | ok rec=0 t=1.84s | ok rec=0 t=0.93s | ok rec=1 t=6.66s | ok rec=4 t=16.33s | ok rec=0 t=1.18s | ok rec=2 t=7.56s | ok rec=0 t=0.51s |
| rockclimb | ok rec=8 t=101.54s | ok rec=38 t=160.95s | incomplete | incomplete | incomplete | ok rec=34 t=165.42s | incomplete | incomplete | incomplete | incomplete |
| schematic | ok rec=0 t=1.05s | ok rec=0 t=6.26s | ok rec=0 t=12.69s | ok rec=5 t=27.99s | ok rec=4 t=18.85s | ok rec=1 t=10.48s | ok rec=6 t=20.45s | ok rec=0 t=1.70s | ok rec=2 t=8.10s | ok rec=0 t=1.04s |
| schematicO3 | ok rec=0 t=0.56s | ok rec=0 t=4.27s | ok rec=0 t=2.25s | ok rec=0 t=1.89s | ok rec=4 t=18.38s | ok rec=1 t=6.67s | ok rec=5 t=18.57s | ok rec=0 t=1.20s | ok rec=2 t=7.57s | ok rec=0 t=0.55s |

### dijkstra (baseline result 3788)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok rec=0 t=0.19s | ok rec=0 t=2.52s | ok rec=0 t=0.15s | ok rec=0 t=1.50s | ok rec=0 t=0.25s | ok rec=0 t=0.31s | ok rec=0 t=0.14s | ok rec=0 t=0.20s | ok rec=2 t=7.21s | ok rec=0 t=0.19s |
| rockclimb | ok rec=10 t=127.05s | ok rec=49 t=200.35s | incomplete | incomplete | incomplete | ok rec=43 t=201.71s | incomplete | incomplete | incomplete | incomplete |
| schematic | ok rec=0 t=0.64s | ok rec=0 t=4.36s | ok rec=0 t=2.37s | ok rec=0 t=2.02s | ok rec=4 t=18.50s | ok rec=1 t=6.88s | ok rec=5 t=18.72s | ok rec=0 t=1.31s | ok rec=2 t=7.66s | ok rec=0 t=0.63s |
| schematicO3 | ok rec=0 t=0.21s | ok rec=0 t=2.53s | ok rec=0 t=0.16s | ok rec=0 t=1.53s | ok rec=0 t=0.28s | ok rec=0 t=0.33s | ok rec=0 t=0.16s | ok rec=0 t=0.83s | ok rec=2 t=7.22s | ok rec=0 t=0.18s |

### qsort (baseline result 9987)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok rec=0 t=0.22s | ok rec=0 t=2.56s | ok rec=0 t=0.18s | ok rec=0 t=1.54s | ok rec=0 t=0.29s | ok rec=0 t=0.35s | ok rec=0 t=0.18s | ok rec=0 t=0.87s | ok rec=2 t=7.23s | ok rec=0 t=0.21s |
| rockclimb | ok rec=2 t=25.57s | ok rec=10 t=43.18s | ok rec=9 t=131.16s | ok rec=35 t=185.31s | ok rec=19 t=112.26s | ok rec=7 t=35.27s | ok rec=18 t=70.18s | ok rec=16 t=108.35s | ok rec=22 t=107.83s | ok rec=14 t=79.29s |
| schematic | ok rec=0 t=0.45s | ok rec=0 t=4.14s | ok rec=0 t=0.40s | ok rec=0 t=1.78s | ok rec=0 t=0.80s | ok rec=1 t=6.57s | ok rec=4 t=16.24s | ok rec=0 t=1.08s | ok rec=2 t=7.45s | ok rec=0 t=0.44s |
| schematicO3 | ok rec=0 t=0.29s | ok rec=0 t=3.98s | ok rec=0 t=0.25s | ok rec=0 t=1.61s | ok rec=0 t=0.36s | ok rec=1 t=6.40s | ok rec=3 t=9.54s | ok rec=0 t=0.94s | ok rec=2 t=7.31s | ok rec=0 t=0.29s |

### activity_recognition (baseline result 64)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok rec=0 t=0.20s | ok rec=0 t=2.52s | ok rec=0 t=0.18s | ok rec=0 t=1.54s | ok rec=0 t=0.28s | ok rec=0 t=0.34s | ok rec=0 t=0.19s | ok rec=0 t=0.86s | ok rec=2 t=7.25s | ok rec=0 t=0.19s |
| rockclimb | ok rec=0 t=1.83s | ok rec=0 t=7.47s | ok rec=3 t=52.01s | ok rec=15 t=80.25s | ok rec=9 t=49.78s | ok rec=1 t=11.41s | ok rec=8 t=30.75s | ok rec=7 t=47.27s | ok rec=7 t=33.02s | ok rec=0 t=1.81s |
| schematic | ok rec=0 t=1.18s | ok rec=0 t=6.38s | ok rec=3 t=39.46s | ok rec=5 t=28.18s | ok rec=5 t=31.10s | ok rec=1 t=10.71s | ok rec=5 t=20.61s | ok rec=1 t=15.87s | ok rec=7 t=32.38s | ok rec=0 t=1.17s |
| schematicO3 | ok rec=0 t=0.27s | ok rec=0 t=3.95s | ok rec=0 t=0.23s | ok rec=0 t=1.58s | ok rec=0 t=0.33s | ok rec=1 t=6.38s | ok rec=3 t=9.53s | ok rec=0 t=0.94s | ok rec=2 t=7.28s | ok rec=0 t=0.26s |

### bitcount (baseline result 14121)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok rec=0 t=0.19s | ok rec=0 t=2.51s | ok rec=0 t=0.16s | ok rec=0 t=1.49s | ok rec=0 t=0.26s | ok rec=0 t=0.25s | ok rec=0 t=0.16s | ok rec=0 t=0.19s | ok rec=2 t=7.23s | ok rec=0 t=0.20s |
| rockclimb | ok rec=5 t=59.67s | ok rec=21 t=95.19s | incomplete | incomplete | incomplete | ok rec=23 t=110.20s | ok rec=51 t=195.27s | ok rec=49 t=324.93s | incomplete | ok rec=35 t=198.62s |
| schematic | ok rec=0 t=0.70s | ok rec=0 t=5.47s | ok rec=0 t=2.99s | ok rec=5 t=26.26s | ok rec=4 t=18.49s | ok rec=1 t=6.89s | ok rec=5 t=18.74s | ok rec=0 t=1.40s | ok rec=2 t=7.75s | ok rec=0 t=0.71s |
| schematicO3 | ok rec=0 t=0.19s | ok rec=0 t=2.52s | ok rec=0 t=0.17s | ok rec=0 t=1.51s | ok rec=0 t=0.26s | ok rec=0 t=0.29s | ok rec=0 t=0.17s | ok rec=0 t=0.83s | ok rec=2 t=7.22s | ok rec=0 t=0.20s |

### chacha20 (baseline result 97)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s |
| rockclimb | ok rec=0 t=0.07s | ok rec=0 t=0.06s | ok rec=0 t=0.04s | ok rec=0 t=0.08s | ok rec=0 t=0.12s | ok rec=0 t=0.07s | ok rec=0 t=0.05s | ok rec=0 t=0.06s | ok rec=2 t=7.11s | ok rec=0 t=0.08s |
| schematic | ok rec=0 t=0.03s | ok rec=0 t=0.03s | ok rec=0 t=0.01s | ok rec=0 t=0.03s | ok rec=0 t=0.03s | ok rec=0 t=0.03s | ok rec=0 t=0.03s | ok rec=0 t=0.03s | ok rec=2 t=7.06s | ok rec=0 t=0.03s |
| schematicO3 | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s |

### sensor_fusion (baseline result 613)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok rec=0 t=0.22s | ok rec=0 t=2.58s | ok rec=0 t=0.20s | ok rec=0 t=1.52s | ok rec=0 t=0.30s | ok rec=0 t=0.36s | ok rec=0 t=0.20s | ok rec=0 t=0.89s | ok rec=2 t=7.25s | ok rec=0 t=0.22s |
| rockclimb | ok rec=0 t=0.21s | ok rec=0 t=2.55s | ok rec=0 t=0.17s | ok rec=0 t=1.55s | ok rec=0 t=0.29s | ok rec=0 t=0.37s | ok rec=0 t=0.17s | ok rec=0 t=0.87s | ok rec=2 t=7.23s | ok rec=0 t=0.20s |
| schematic | ok rec=0 t=0.20s | ok rec=0 t=2.55s | ok rec=0 t=0.16s | ok rec=0 t=1.54s | ok rec=0 t=0.27s | ok rec=0 t=0.34s | ok rec=0 t=0.16s | ok rec=0 t=0.83s | ok rec=2 t=7.22s | ok rec=0 t=0.19s |
| schematicO3 | ok rec=0 t=0.39s | ok rec=0 t=4.08s | ok rec=0 t=0.35s | ok rec=0 t=1.71s | ok rec=0 t=0.74s | ok rec=1 t=6.50s | ok rec=4 t=16.21s | ok rec=0 t=1.06s | ok rec=2 t=7.40s | ok rec=0 t=0.36s |

### poly1305 (baseline result 108)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s |
| rockclimb | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s |
| schematic | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s |
| schematicO3 | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s |

### cuckoo_filter (baseline result 256)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s | ok rec=0 t=0.00s |
| rockclimb | ok rec=0 t=0.29s | ok rec=0 t=4.01s | ok rec=0 t=0.26s | ok rec=0 t=1.67s | ok rec=0 t=0.68s | ok rec=1 t=6.47s | ok rec=3 t=9.57s | ok rec=0 t=0.97s | ok rec=2 t=7.32s | ok rec=0 t=0.28s |
| schematic | ok rec=0 t=0.12s | ok rec=0 t=2.45s | ok rec=0 t=0.09s | ok rec=0 t=1.36s | ok rec=0 t=0.22s | ok rec=0 t=0.14s | ok rec=0 t=0.10s | ok rec=0 t=0.12s | ok rec=2 t=7.15s | ok rec=0 t=0.12s |
| schematicO3 | ok rec=0 t=0.03s | ok rec=0 t=0.03s | ok rec=0 t=0.01s | ok rec=0 t=0.03s | ok rec=0 t=0.03s | ok rec=0 t=0.03s | ok rec=0 t=0.01s | ok rec=0 t=0.03s | ok rec=2 t=7.06s | ok rec=0 t=0.03s |

### sha256_fixed (baseline result 244)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok rec=0 t=0.04s | ok rec=0 t=0.03s | ok rec=0 t=0.03s | ok rec=0 t=0.04s | ok rec=0 t=0.03s | ok rec=0 t=0.04s | ok rec=0 t=0.03s | ok rec=0 t=0.03s | ok rec=2 t=7.06s | ok rec=0 t=0.04s |
| rockclimb | ok rec=0 t=0.26s | ok rec=0 t=3.98s | ok rec=0 t=0.23s | ok rec=0 t=1.63s | ok rec=0 t=0.36s | ok rec=1 t=6.43s | ok rec=3 t=9.53s | ok rec=0 t=0.95s | ok rec=2 t=7.28s | ok rec=0 t=0.25s |
| schematic | ok rec=0 t=0.09s | ok rec=0 t=0.95s | ok rec=0 t=0.08s | ok rec=0 t=0.10s | ok rec=0 t=0.13s | ok rec=0 t=0.09s | ok rec=0 t=0.08s | ok rec=0 t=0.09s | ok rec=2 t=7.12s | ok rec=0 t=0.09s |
| schematicO3 | ok rec=0 t=0.03s | ok rec=0 t=0.03s | ok rec=0 t=0.02s | ok rec=0 t=0.03s | ok rec=0 t=0.03s | ok rec=0 t=0.03s | ok rec=0 t=0.02s | ok rec=0 t=0.03s | ok rec=2 t=7.06s | ok rec=0 t=0.03s |

### stringsearch_fixed (baseline result 20)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok rec=0 t=0.53s | ok rec=0 t=4.21s | ok rec=0 t=2.21s | ok rec=0 t=1.84s | ok rec=0 t=0.88s | ok rec=1 t=6.62s | ok rec=4 t=16.32s | ok rec=0 t=1.17s | ok rec=2 t=7.58s | ok rec=0 t=0.53s |
| rockclimb | ok rec=12 t=151.99s | ok rec=60 t=237.86s | incomplete | incomplete | incomplete | ok rec=46 t=222.98s | incomplete | incomplete | incomplete | incomplete |
| schematic | ok rec=0 t=0.87s | ok rec=0 t=5.63s | ok rec=0 t=12.62s | ok rec=5 t=27.83s | ok rec=4 t=18.66s | ok rec=1 t=10.31s | ok rec=6 t=20.32s | ok rec=0 t=1.52s | ok rec=2 t=7.90s | ok rec=0 t=0.86s |
| schematicO3 | ok rec=0 t=0.55s | ok rec=0 t=4.25s | ok rec=0 t=2.24s | ok rec=0 t=1.88s | ok rec=4 t=18.37s | ok rec=1 t=6.69s | ok rec=5 t=18.59s | ok rec=0 t=1.21s | ok rec=2 t=7.57s | ok rec=0 t=0.56s |


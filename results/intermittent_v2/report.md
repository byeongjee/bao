# Intermittent-power results (v2)

13 benchmarks x 4 algorithms x 10 traces (benchmarks/traces/1..10.csv replayed 10x faster than recorded), board cap 11.5 uF, 560 Ohm series resistor, wait threshold 3.0 V, halt-mode wait, cpu 16 MHz, no device-debug.

Cell: `ok` = completed (stop pulse + __nvm_done) and return code equals the uninstrumented baseline (`WRONG` otherwise); `incomplete` = did not finish within the trace; w = waits (cnt_wait: boundary/boot found VCC below 3.0 V and slept); rec = recovery boots (cnt_recovery, real brownouts); t = execution time incl. wait/outage time (Saleae start->stop pulse).

## Totals per algorithm

| algo | runs | complete | correct (of complete) | incomplete | mean waits | mean rec | mean t (s) |
|---|---|---|---|---|---|---|---|
| milp | 130 | 130 | 130 | 0 | 9.0 | 0.1 | 0.87 |
| rockclimb | 130 | 129 | 129 | 1 | 231.7 | 9.3 | 32.23 |
| schematic | 130 | 130 | 130 | 0 | 38.6 | 1.2 | 4.95 |
| schematicO3 | 130 | 130 | 130 | 0 | 11.3 | 0.1 | 1.14 |

## Per benchmark: complete / correct / total waits / total rec / mean t (s)

| benchmark (baseline result) | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes (107) | 10/10 / 10/10 / 70 / 0 / 0.59 | 10/10 / 10/10 / 673 / 23 / 8.97 | 10/10 / 10/10 / 1455 / 61 / 21.17 | 10/10 / 10/10 / 122 / 1 / 1.11 |
| crc (39423) | 10/10 / 10/10 / 46 / 1 / 0.47 | 10/10 / 10/10 / 467 / 12 / 5.49 | 10/10 / 10/10 / 447 / 14 / 5.60 | 10/10 / 10/10 / 42 / 1 / 0.46 |
| rsa (32) | 10/10 / 10/10 / 228 / 5 / 2.52 | 10/10 / 10/10 / 6190 / 246 / 89.78 | 10/10 / 10/10 / 531 / 17 / 6.87 | 10/10 / 10/10 / 254 / 7 / 3.05 |
| dijkstra (3788) | 10/10 / 10/10 / 103 / 0 / 0.88 | 10/10 / 10/10 / 7658 / 306 / 109.99 | 10/10 / 10/10 / 473 / 18 / 6.28 | 10/10 / 10/10 / 122 / 0 / 1.13 |
| qsort (9987) | 10/10 / 10/10 / 119 / 0 / 1.27 | 10/10 / 10/10 / 1571 / 58 / 21.95 | 10/10 / 10/10 / 246 / 5 / 2.56 | 10/10 / 10/10 / 175 / 2 / 1.76 |
| activity_recognition (64) | 10/10 / 10/10 / 129 / 1 / 1.29 | 10/10 / 10/10 / 729 / 26 / 9.94 | 10/10 / 10/10 / 586 / 17 / 7.35 | 10/10 / 10/10 / 173 / 2 / 1.77 |
| bitcount (14121) | 10/10 / 10/10 / 90 / 0 / 0.87 | 10/10 / 10/10 / 4208 / 165 / 59.55 | 10/10 / 10/10 / 362 / 9 / 4.23 | 10/10 / 10/10 / 103 / 0 / 1.00 |
| chacha20 (97) | 10/10 / 10/10 / 11 / 0 / 0.00 | 10/10 / 10/10 / 35 / 0 / 0.37 | 10/10 / 10/10 / 21 / 0 / 0.11 | 10/10 / 10/10 / 11 / 0 / 0.00 |
| sensor_fusion (613) | 10/10 / 10/10 / 114 / 0 / 1.06 | 10/10 / 10/10 / 128 / 0 / 1.30 | 10/10 / 10/10 / 167 / 2 / 1.54 | 10/10 / 10/10 / 168 / 1 / 1.53 |
| poly1305 (108) | 10/10 / 10/10 / 11 / 0 / 0.00 | 10/10 / 10/10 / 11 / 0 / 0.00 | 10/10 / 10/10 / 11 / 0 / 0.00 | 10/10 / 10/10 / 11 / 0 / 0.00 |
| cuckoo_filter (256) | 10/10 / 10/10 / 20 / 0 / 0.10 | 10/10 / 10/10 / 119 / 0 / 1.06 | 10/10 / 10/10 / 72 / 0 / 0.58 | 10/10 / 10/10 / 26 / 0 / 0.18 |
| sha256_fixed (244) | 10/10 / 10/10 / 26 / 1 / 0.28 | 10/10 / 10/10 / 110 / 0 / 0.87 | 10/10 / 10/10 / 46 / 0 / 0.46 | 10/10 / 10/10 / 31 / 0 / 0.37 |
| stringsearch_fixed (20) | 10/10 / 10/10 / 208 / 2 / 1.98 | 9/10 / 9/9 / 7991 / 360 / 118.34 | 10/10 / 10/10 / 600 / 19 / 7.63 | 10/10 / 10/10 / 232 / 5 / 2.45 |

## Per trace: waits by benchmark x algorithm (`inc` = incomplete)

### trace 1

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 7 | 45 | 114 | 10 |
| crc | 4 | 29 | 35 | 4 |
| rsa | 18 | 414 | 45 | 20 |
| dijkstra | 9 | 517 | 39 | 10 |
| qsort | 11 | 105 | 18 | 13 |
| activity_recognition | 11 | 50 | 45 | 14 |
| bitcount | 8 | 282 | 30 | 9 |
| chacha20 | 1 | 2 | 2 | 1 |
| sensor_fusion | 9 | 10 | 14 | 12 |
| poly1305 | 1 | 1 | 1 | 1 |
| cuckoo_filter | 2 | 8 | 6 | 2 |
| sha256_fixed | 2 | 7 | 4 | 3 |
| stringsearch_fixed | 15 | 609 | 46 | 17 |

### trace 2

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 7 | 64 | 134 | 10 |
| crc | 5 | 39 | 40 | 4 |
| rsa | 20 | 542 | 49 | 22 |
| dijkstra | 10 | 683 | 44 | 12 |
| qsort | 11 | 146 | 23 | 15 |
| activity_recognition | 13 | 64 | 53 | 15 |
| bitcount | 10 | 377 | 34 | 10 |
| chacha20 | 1 | 4 | 2 | 1 |
| sensor_fusion | 12 | 13 | 16 | 16 |
| poly1305 | 1 | 1 | 1 | 1 |
| cuckoo_filter | 2 | 11 | 8 | 3 |
| sha256_fixed | 3 | 12 | 5 | 3 |
| stringsearch_fixed | 21 | 813 | 59 | 21 |

### trace 3

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 7 | 106 | 198 | 16 |
| crc | 4 | 84 | 69 | 4 |
| rsa | 30 | 994 | 71 | 36 |
| dijkstra | 11 | 1235 | 60 | 15 |
| qsort | 11 | 245 | 30 | 21 |
| activity_recognition | 12 | 112 | 87 | 21 |
| bitcount | 8 | 681 | 42 | 9 |
| chacha20 | 1 | 3 | 2 | 1 |
| sensor_fusion | 13 | 14 | 20 | 21 |
| poly1305 | 1 | 1 | 1 | 1 |
| cuckoo_filter | 2 | 15 | 6 | 2 |
| sha256_fixed | 2 | 13 | 3 | 3 |
| stringsearch_fixed | 25 | 1513 | 84 | 29 |

### trace 4

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 7 | 84 | 152 | 13 |
| crc | 6 | 56 | 48 | 5 |
| rsa | 28 | 771 | 51 | 27 |
| dijkstra | 9 | 953 | 47 | 12 |
| qsort | 14 | 193 | 31 | 19 |
| activity_recognition | 12 | 95 | 62 | 18 |
| bitcount | 8 | 516 | 41 | 10 |
| chacha20 | 1 | 4 | 2 | 1 |
| sensor_fusion | 12 | 13 | 18 | 20 |
| poly1305 | 1 | 1 | 1 | 1 |
| cuckoo_filter | 2 | 17 | 7 | 3 |
| sha256_fixed | 4 | 11 | 5 | 3 |
| stringsearch_fixed | 25 | inc | 63 | 29 |

### trace 5

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 7 | 61 | 151 | 13 |
| crc | 4 | 45 | 44 | 4 |
| rsa | 24 | 610 | 52 | 27 |
| dijkstra | 11 | 721 | 52 | 12 |
| qsort | 11 | 148 | 25 | 18 |
| activity_recognition | 13 | 70 | 56 | 18 |
| bitcount | 9 | 404 | 35 | 10 |
| chacha20 | 1 | 3 | 2 | 1 |
| sensor_fusion | 12 | 13 | 17 | 18 |
| poly1305 | 1 | 1 | 1 | 1 |
| cuckoo_filter | 2 | 12 | 7 | 2 |
| sha256_fixed | 2 | 10 | 4 | 3 |
| stringsearch_fixed | 22 | 849 | 61 | 23 |

### trace 6

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 6 | 57 | 131 | 13 |
| crc | 4 | 39 | 38 | 4 |
| rsa | 18 | 521 | 46 | 23 |
| dijkstra | 10 | 644 | 41 | 14 |
| qsort | 12 | 132 | 23 | 18 |
| activity_recognition | 14 | 64 | 51 | 18 |
| bitcount | 10 | 354 | 34 | 11 |
| chacha20 | 1 | 3 | 2 | 1 |
| sensor_fusion | 11 | 14 | 16 | 16 |
| poly1305 | 1 | 1 | 1 | 1 |
| cuckoo_filter | 2 | 11 | 8 | 3 |
| sha256_fixed | 2 | 12 | 3 | 3 |
| stringsearch_fixed | 19 | 761 | 54 | 22 |

### trace 7

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 7 | 59 | 132 | 12 |
| crc | 5 | 41 | 42 | 4 |
| rsa | 22 | 535 | 50 | 23 |
| dijkstra | 12 | 656 | 47 | 13 |
| qsort | 12 | 140 | 23 | 18 |
| activity_recognition | 14 | 64 | 56 | 19 |
| bitcount | 9 | 371 | 35 | 11 |
| chacha20 | 1 | 3 | 2 | 1 |
| sensor_fusion | 12 | 13 | 16 | 17 |
| poly1305 | 1 | 1 | 1 | 1 |
| cuckoo_filter | 1 | 11 | 7 | 2 |
| sha256_fixed | 3 | 12 | 5 | 3 |
| stringsearch_fixed | 21 | 795 | 56 | 21 |

### trace 8

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 8 | 82 | 178 | 14 |
| crc | 5 | 57 | 53 | 5 |
| rsa | 26 | 809 | 68 | 31 |
| dijkstra | 12 | 1007 | 59 | 14 |
| qsort | 15 | 202 | 27 | 24 |
| activity_recognition | 16 | 91 | 75 | 21 |
| bitcount | 12 | 546 | 42 | 14 |
| chacha20 | 2 | 7 | 3 | 2 |
| sensor_fusion | 14 | 16 | 19 | 20 |
| poly1305 | 2 | 2 | 2 | 2 |
| cuckoo_filter | 3 | 17 | 11 | 4 |
| sha256_fixed | 3 | 16 | 8 | 4 |
| stringsearch_fixed | 26 | 1182 | 74 | 28 |

### trace 9

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 7 | 59 | 130 | 10 |
| crc | 5 | 38 | 37 | 4 |
| rsa | 20 | 485 | 48 | 21 |
| dijkstra | 10 | 606 | 42 | 11 |
| qsort | 12 | 129 | 24 | 16 |
| activity_recognition | 13 | 60 | 51 | 16 |
| bitcount | 8 | 329 | 34 | 10 |
| chacha20 | 1 | 4 | 2 | 1 |
| sensor_fusion | 10 | 12 | 17 | 16 |
| poly1305 | 1 | 1 | 1 | 1 |
| cuckoo_filter | 2 | 10 | 7 | 3 |
| sha256_fixed | 3 | 10 | 5 | 3 |
| stringsearch_fixed | 19 | 728 | 51 | 20 |

### trace 10

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 7 | 56 | 135 | 11 |
| crc | 4 | 39 | 41 | 4 |
| rsa | 22 | 509 | 51 | 24 |
| dijkstra | 9 | 636 | 42 | 9 |
| qsort | 10 | 131 | 22 | 13 |
| activity_recognition | 11 | 59 | 50 | 13 |
| bitcount | 8 | 348 | 35 | 9 |
| chacha20 | 1 | 2 | 2 | 1 |
| sensor_fusion | 9 | 10 | 14 | 12 |
| poly1305 | 1 | 1 | 1 | 1 |
| cuckoo_filter | 2 | 7 | 5 | 2 |
| sha256_fixed | 2 | 7 | 4 | 3 |
| stringsearch_fixed | 15 | 741 | 52 | 22 |

## Per benchmark x algorithm x trace

### aes (baseline result 107)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=7 rec=0 t=0.09s | ok w=7 rec=0 t=0.42s | ok w=7 rec=0 t=0.12s | ok w=7 rec=0 t=0.83s | ok w=7 rec=0 t=0.10s | ok w=6 rec=0 t=0.66s | ok w=7 rec=0 t=1.86s | ok w=8 rec=0 t=0.85s | ok w=7 rec=0 t=0.89s | ok w=7 rec=0 t=0.09s |
| rockclimb | ok w=45 rec=1 t=2.69s | ok w=64 rec=1 t=4.35s | ok w=106 rec=1 t=3.97s | ok w=84 rec=0 t=16.53s | ok w=61 rec=3 t=11.20s | ok w=57 rec=1 t=5.16s | ok w=59 rec=0 t=9.03s | ok w=82 rec=7 t=11.59s | ok w=59 rec=0 t=13.33s | ok w=56 rec=9 t=11.82s |
| schematic | ok w=114 rec=3 t=7.78s | ok w=134 rec=2 t=9.38s | ok w=198 rec=2 t=8.01s | ok w=152 rec=0 t=37.33s | ok w=151 rec=9 t=27.98s | ok w=131 rec=2 t=11.54s | ok w=132 rec=0 t=22.50s | ok w=178 rec=18 t=28.64s | ok w=130 rec=4 t=30.84s | ok w=135 rec=21 t=27.66s |
| schematicO3 | ok w=10 rec=0 t=0.13s | ok w=10 rec=0 t=0.56s | ok w=16 rec=0 t=0.21s | ok w=13 rec=1 t=3.27s | ok w=13 rec=0 t=1.88s | ok w=13 rec=0 t=1.03s | ok w=12 rec=0 t=2.05s | ok w=14 rec=0 t=0.89s | ok w=10 rec=0 t=0.92s | ok w=11 rec=0 t=0.14s |

### crc (baseline result 39423)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=4 rec=0 t=0.04s | ok w=5 rec=0 t=0.39s | ok w=4 rec=0 t=0.04s | ok w=6 rec=1 t=0.80s | ok w=4 rec=0 t=0.04s | ok w=4 rec=0 t=0.04s | ok w=5 rec=0 t=1.62s | ok w=5 rec=0 t=0.78s | ok w=5 rec=0 t=0.85s | ok w=4 rec=0 t=0.04s |
| rockclimb | ok w=29 rec=0 t=1.01s | ok w=39 rec=0 t=1.71s | ok w=84 rec=0 t=1.21s | ok w=56 rec=0 t=11.32s | ok w=45 rec=2 t=8.08s | ok w=39 rec=0 t=1.64s | ok w=41 rec=0 t=6.54s | ok w=57 rec=4 t=7.03s | ok w=38 rec=0 t=8.40s | ok w=39 rec=6 t=7.91s |
| schematic | ok w=35 rec=1 t=2.52s | ok w=40 rec=0 t=1.69s | ok w=69 rec=0 t=1.06s | ok w=48 rec=1 t=11.26s | ok w=44 rec=2 t=8.06s | ok w=38 rec=0 t=1.63s | ok w=42 rec=0 t=6.52s | ok w=53 rec=4 t=7.00s | ok w=37 rec=0 t=8.36s | ok w=41 rec=6 t=7.91s |
| schematicO3 | ok w=4 rec=0 t=0.05s | ok w=4 rec=0 t=0.39s | ok w=4 rec=0 t=0.05s | ok w=5 rec=1 t=0.79s | ok w=4 rec=0 t=0.05s | ok w=4 rec=0 t=0.05s | ok w=4 rec=0 t=1.60s | ok w=5 rec=0 t=0.78s | ok w=4 rec=0 t=0.84s | ok w=4 rec=0 t=0.05s |

### rsa (baseline result 32)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=18 rec=0 t=0.84s | ok w=20 rec=0 t=0.74s | ok w=30 rec=0 t=0.37s | ok w=28 rec=0 t=6.04s | ok w=24 rec=1 t=3.20s | ok w=18 rec=0 t=1.12s | ok w=22 rec=0 t=3.08s | ok w=26 rec=1 t=2.39s | ok w=20 rec=0 t=3.43s | ok w=22 rec=3 t=3.98s |
| rockclimb | ok w=414 rec=13 t=32.90s | ok w=542 rec=10 t=40.81s | ok w=994 rec=9 t=36.69s | ok w=771 rec=0 t=171.09s | ok w=610 rec=38 t=119.63s | ok w=521 rec=10 t=50.20s | ok w=535 rec=0 t=85.59s | ok w=809 rec=82 t=127.12s | ok w=485 rec=0 t=123.36s | ok w=509 rec=84 t=110.38s |
| schematic | ok w=45 rec=1 t=2.66s | ok w=49 rec=0 t=1.94s | ok w=71 rec=0 t=1.19s | ok w=51 rec=0 t=11.94s | ok w=52 rec=3 t=9.36s | ok w=46 rec=1 t=3.78s | ok w=50 rec=0 t=8.79s | ok w=68 rec=6 t=10.11s | ok w=48 rec=0 t=10.87s | ok w=51 rec=6 t=8.04s |
| schematicO3 | ok w=20 rec=0 t=0.86s | ok w=22 rec=0 t=0.76s | ok w=36 rec=0 t=0.44s | ok w=27 rec=0 t=6.07s | ok w=27 rec=1 t=4.95s | ok w=23 rec=0 t=1.17s | ok w=23 rec=0 t=3.49s | ok w=31 rec=3 t=5.38s | ok w=21 rec=0 t=3.42s | ok w=24 rec=3 t=3.99s |

### dijkstra (baseline result 3788)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=9 rec=0 t=0.11s | ok w=10 rec=0 t=0.56s | ok w=11 rec=0 t=0.15s | ok w=9 rec=0 t=1.46s | ok w=11 rec=0 t=1.85s | ok w=10 rec=0 t=0.72s | ok w=12 rec=0 t=2.02s | ok w=12 rec=0 t=0.88s | ok w=10 rec=0 t=0.92s | ok w=9 rec=0 t=0.12s |
| rockclimb | ok w=517 rec=16 t=41.21s | ok w=683 rec=13 t=51.60s | ok w=1235 rec=12 t=47.52s | ok w=953 rec=0 t=197.35s | ok w=721 rec=45 t=141.39s | ok w=644 rec=12 t=62.62s | ok w=656 rec=0 t=107.45s | ok w=1007 rec=103 t=159.43s | ok w=606 rec=0 t=153.33s | ok w=636 rec=105 t=137.96s |
| schematic | ok w=39 rec=1 t=2.59s | ok w=44 rec=0 t=1.89s | ok w=60 rec=0 t=1.06s | ok w=47 rec=0 t=11.28s | ok w=52 rec=3 t=9.34s | ok w=41 rec=0 t=1.70s | ok w=47 rec=0 t=8.57s | ok w=59 rec=6 t=10.03s | ok w=42 rec=2 t=8.43s | ok w=42 rec=6 t=7.95s |
| schematicO3 | ok w=10 rec=0 t=0.13s | ok w=12 rec=0 t=0.62s | ok w=15 rec=0 t=0.20s | ok w=12 rec=0 t=3.39s | ok w=12 rec=0 t=1.88s | ok w=14 rec=0 t=1.04s | ok w=13 rec=0 t=2.05s | ok w=14 rec=0 t=0.90s | ok w=11 rec=0 t=0.94s | ok w=9 rec=0 t=0.12s |

### qsort (baseline result 9987)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=11 rec=0 t=0.14s | ok w=11 rec=0 t=0.61s | ok w=11 rec=0 t=0.15s | ok w=14 rec=0 t=3.41s | ok w=11 rec=0 t=1.87s | ok w=12 rec=0 t=0.89s | ok w=12 rec=0 t=2.06s | ok w=15 rec=0 t=0.91s | ok w=12 rec=0 t=2.51s | ok w=10 rec=0 t=0.13s |
| rockclimb | ok w=105 rec=3 t=7.72s | ok w=146 rec=2 t=9.49s | ok w=245 rec=2 t=8.51s | ok w=193 rec=0 t=42.74s | ok w=148 rec=9 t=27.97s | ok w=132 rec=2 t=11.52s | ok w=140 rec=0 t=22.90s | ok w=202 rec=19 t=30.12s | ok w=129 rec=0 t=30.87s | ok w=131 rec=21 t=27.61s |
| schematic | ok w=18 rec=0 t=0.84s | ok w=23 rec=0 t=0.74s | ok w=30 rec=0 t=0.36s | ok w=31 rec=0 t=6.04s | ok w=25 rec=1 t=3.20s | ok w=23 rec=0 t=1.15s | ok w=23 rec=0 t=3.49s | ok w=27 rec=1 t=2.41s | ok w=24 rec=0 t=3.43s | ok w=22 rec=3 t=3.97s |
| schematicO3 | ok w=13 rec=0 t=0.17s | ok w=15 rec=0 t=0.66s | ok w=21 rec=0 t=0.26s | ok w=19 rec=0 t=3.44s | ok w=18 rec=1 t=3.13s | ok w=18 rec=0 t=1.08s | ok w=18 rec=0 t=3.04s | ok w=24 rec=1 t=2.35s | ok w=16 rec=0 t=3.36s | ok w=13 rec=0 t=0.17s |

### activity_recognition (baseline result 64)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=11 rec=0 t=0.14s | ok w=13 rec=0 t=0.63s | ok w=12 rec=0 t=0.17s | ok w=12 rec=0 t=3.39s | ok w=13 rec=0 t=1.90s | ok w=14 rec=0 t=1.05s | ok w=14 rec=0 t=2.05s | ok w=16 rec=0 t=0.91s | ok w=13 rec=1 t=2.50s | ok w=11 rec=0 t=0.14s |
| rockclimb | ok w=50 rec=1 t=3.38s | ok w=64 rec=1 t=4.54s | ok w=112 rec=1 t=4.07s | ok w=95 rec=0 t=19.13s | ok w=70 rec=4 t=12.48s | ok w=64 rec=1 t=5.83s | ok w=64 rec=0 t=10.03s | ok w=91 rec=9 t=14.62s | ok w=60 rec=0 t=13.40s | ok w=59 rec=9 t=11.88s |
| schematic | ok w=45 rec=1 t=2.66s | ok w=53 rec=0 t=1.95s | ok w=87 rec=0 t=1.28s | ok w=62 rec=0 t=13.91s | ok w=56 rec=3 t=11.16s | ok w=51 rec=1 t=4.62s | ok w=56 rec=0 t=8.83s | ok w=75 rec=6 t=10.12s | ok w=51 rec=0 t=10.90s | ok w=50 rec=6 t=8.03s |
| schematicO3 | ok w=14 rec=0 t=0.18s | ok w=15 rec=0 t=0.65s | ok w=21 rec=0 t=0.26s | ok w=18 rec=0 t=3.45s | ok w=18 rec=1 t=3.13s | ok w=18 rec=0 t=1.10s | ok w=19 rec=0 t=3.05s | ok w=21 rec=1 t=2.34s | ok w=16 rec=0 t=3.36s | ok w=13 rec=0 t=0.17s |

### bitcount (baseline result 14121)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=8 rec=0 t=0.11s | ok w=10 rec=0 t=0.56s | ok w=8 rec=0 t=0.13s | ok w=8 rec=0 t=1.45s | ok w=9 rec=0 t=1.84s | ok w=10 rec=0 t=0.70s | ok w=9 rec=0 t=2.02s | ok w=12 rec=0 t=0.87s | ok w=8 rec=0 t=0.90s | ok w=8 rec=0 t=0.11s |
| rockclimb | ok w=282 rec=9 t=22.72s | ok w=377 rec=7 t=28.09s | ok w=681 rec=6 t=24.73s | ok w=516 rec=0 t=105.59s | ok w=404 rec=25 t=79.35s | ok w=354 rec=6 t=32.45s | ok w=371 rec=0 t=58.72s | ok w=546 rec=55 t=85.50s | ok w=329 rec=0 t=83.39s | ok w=348 rec=57 t=74.92s |
| schematic | ok w=30 rec=0 t=0.99s | ok w=34 rec=0 t=1.44s | ok w=42 rec=0 t=0.69s | ok w=41 rec=0 t=8.68s | ok w=35 rec=2 t=6.24s | ok w=34 rec=0 t=1.49s | ok w=35 rec=0 t=5.53s | ok w=42 rec=3 t=5.52s | ok w=34 rec=1 t=7.62s | ok w=35 rec=3 t=4.14s |
| schematicO3 | ok w=9 rec=0 t=0.12s | ok w=10 rec=0 t=0.56s | ok w=9 rec=0 t=0.14s | ok w=10 rec=0 t=2.63s | ok w=10 rec=0 t=1.86s | ok w=11 rec=0 t=0.72s | ok w=11 rec=0 t=2.03s | ok w=14 rec=0 t=0.88s | ok w=10 rec=0 t=0.92s | ok w=9 rec=0 t=0.12s |

### chacha20 (baseline result 97)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=2 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s |
| rockclimb | ok w=2 rec=0 t=0.02s | ok w=4 rec=0 t=0.25s | ok w=3 rec=0 t=0.03s | ok w=4 rec=0 t=0.78s | ok w=3 rec=0 t=0.03s | ok w=3 rec=0 t=0.03s | ok w=3 rec=0 t=0.95s | ok w=7 rec=0 t=0.77s | ok w=4 rec=0 t=0.83s | ok w=2 rec=0 t=0.01s |
| schematic | ok w=2 rec=0 t=0.01s | ok w=2 rec=0 t=0.23s | ok w=2 rec=0 t=0.01s | ok w=2 rec=0 t=0.65s | ok w=2 rec=0 t=0.01s | ok w=2 rec=0 t=0.01s | ok w=2 rec=0 t=0.01s | ok w=3 rec=0 t=0.04s | ok w=2 rec=0 t=0.11s | ok w=2 rec=0 t=0.01s |
| schematicO3 | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=2 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s |

### sensor_fusion (baseline result 613)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=9 rec=0 t=0.11s | ok w=12 rec=0 t=0.57s | ok w=13 rec=0 t=0.17s | ok w=12 rec=0 t=3.27s | ok w=12 rec=0 t=1.86s | ok w=11 rec=0 t=0.71s | ok w=12 rec=0 t=2.04s | ok w=14 rec=0 t=0.88s | ok w=10 rec=0 t=0.92s | ok w=9 rec=0 t=0.11s |
| rockclimb | ok w=10 rec=0 t=0.13s | ok w=13 rec=0 t=0.62s | ok w=14 rec=0 t=0.18s | ok w=13 rec=0 t=3.41s | ok w=13 rec=0 t=1.89s | ok w=14 rec=0 t=1.04s | ok w=13 rec=0 t=2.06s | ok w=16 rec=0 t=0.89s | ok w=12 rec=0 t=2.62s | ok w=10 rec=0 t=0.13s |
| schematic | ok w=14 rec=0 t=0.18s | ok w=16 rec=0 t=0.64s | ok w=20 rec=0 t=0.25s | ok w=18 rec=0 t=3.45s | ok w=17 rec=1 t=3.11s | ok w=16 rec=0 t=1.07s | ok w=16 rec=0 t=2.21s | ok w=19 rec=0 t=0.95s | ok w=17 rec=1 t=3.34s | ok w=14 rec=0 t=0.18s |
| schematicO3 | ok w=12 rec=0 t=0.15s | ok w=16 rec=0 t=0.63s | ok w=21 rec=0 t=0.25s | ok w=20 rec=0 t=3.44s | ok w=18 rec=1 t=3.11s | ok w=16 rec=0 t=1.07s | ok w=17 rec=0 t=2.21s | ok w=20 rec=0 t=0.94s | ok w=16 rec=0 t=3.35s | ok w=12 rec=0 t=0.15s |

### poly1305 (baseline result 108)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=2 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s |
| rockclimb | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=2 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s |
| schematic | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=2 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s |
| schematicO3 | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=2 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s | ok w=1 rec=0 t=0.00s |

### cuckoo_filter (baseline result 256)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=2 rec=0 t=0.01s | ok w=2 rec=0 t=0.09s | ok w=2 rec=0 t=0.01s | ok w=2 rec=0 t=0.65s | ok w=2 rec=0 t=0.01s | ok w=2 rec=0 t=0.01s | ok w=1 rec=0 t=0.00s | ok w=3 rec=0 t=0.04s | ok w=2 rec=0 t=0.11s | ok w=2 rec=0 t=0.01s |
| rockclimb | ok w=8 rec=0 t=0.10s | ok w=11 rec=0 t=0.54s | ok w=15 rec=0 t=0.18s | ok w=17 rec=0 t=3.27s | ok w=12 rec=0 t=1.87s | ok w=11 rec=0 t=0.70s | ok w=11 rec=0 t=2.04s | ok w=17 rec=0 t=0.89s | ok w=10 rec=0 t=0.91s | ok w=7 rec=0 t=0.09s |
| schematic | ok w=6 rec=0 t=0.07s | ok w=8 rec=0 t=0.43s | ok w=6 rec=0 t=0.08s | ok w=7 rec=0 t=0.83s | ok w=7 rec=0 t=0.08s | ok w=8 rec=0 t=0.67s | ok w=7 rec=0 t=1.85s | ok w=11 rec=0 t=0.84s | ok w=7 rec=0 t=0.88s | ok w=5 rec=0 t=0.06s |
| schematicO3 | ok w=2 rec=0 t=0.01s | ok w=3 rec=0 t=0.23s | ok w=2 rec=0 t=0.01s | ok w=3 rec=0 t=0.66s | ok w=2 rec=0 t=0.01s | ok w=3 rec=0 t=0.03s | ok w=2 rec=0 t=0.01s | ok w=4 rec=0 t=0.04s | ok w=3 rec=0 t=0.72s | ok w=2 rec=0 t=0.01s |

### sha256_fixed (baseline result 244)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=2 rec=0 t=0.02s | ok w=3 rec=0 t=0.24s | ok w=2 rec=0 t=0.02s | ok w=4 rec=1 t=0.66s | ok w=2 rec=0 t=0.02s | ok w=2 rec=0 t=0.02s | ok w=3 rec=0 t=0.95s | ok w=3 rec=0 t=0.04s | ok w=3 rec=0 t=0.83s | ok w=2 rec=0 t=0.02s |
| rockclimb | ok w=7 rec=0 t=0.08s | ok w=12 rec=0 t=0.55s | ok w=13 rec=0 t=0.15s | ok w=11 rec=0 t=1.45s | ok w=10 rec=0 t=1.84s | ok w=12 rec=0 t=0.70s | ok w=12 rec=0 t=2.03s | ok w=16 rec=0 t=0.88s | ok w=10 rec=0 t=0.90s | ok w=7 rec=0 t=0.08s |
| schematic | ok w=4 rec=0 t=0.04s | ok w=5 rec=0 t=0.39s | ok w=3 rec=0 t=0.03s | ok w=5 rec=0 t=0.79s | ok w=4 rec=0 t=0.04s | ok w=3 rec=0 t=0.03s | ok w=5 rec=0 t=1.61s | ok w=8 rec=0 t=0.80s | ok w=5 rec=0 t=0.86s | ok w=4 rec=0 t=0.04s |
| schematicO3 | ok w=3 rec=0 t=0.03s | ok w=3 rec=0 t=0.24s | ok w=3 rec=0 t=0.03s | ok w=3 rec=0 t=0.77s | ok w=3 rec=0 t=0.03s | ok w=3 rec=0 t=0.03s | ok w=3 rec=0 t=0.95s | ok w=4 rec=0 t=0.76s | ok w=3 rec=0 t=0.82s | ok w=3 rec=0 t=0.03s |

### stringsearch_fixed (baseline result 20)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=15 rec=0 t=0.20s | ok w=21 rec=0 t=0.71s | ok w=25 rec=0 t=0.31s | ok w=25 rec=0 t=5.24s | ok w=22 rec=1 t=3.16s | ok w=19 rec=0 t=1.11s | ok w=21 rec=0 t=3.07s | ok w=26 rec=1 t=2.39s | ok w=19 rec=0 t=3.38s | ok w=15 rec=0 t=0.20s |
| rockclimb | ok w=609 rec=19 t=48.77s | ok w=813 rec=15 t=60.65s | ok w=1513 rec=14 t=56.54s | incomplete | ok w=849 rec=54 t=167.44s | ok w=761 rec=14 t=73.33s | ok w=795 rec=0 t=128.76s | ok w=1182 rec=121 t=187.15s | ok w=728 rec=0 t=180.83s | ok w=741 rec=123 t=161.63s |
| schematic | ok w=46 rec=1 t=2.68s | ok w=59 rec=1 t=4.33s | ok w=84 rec=0 t=1.31s | ok w=63 rec=0 t=13.94s | ok w=61 rec=3 t=11.19s | ok w=54 rec=1 t=4.70s | ok w=56 rec=0 t=9.03s | ok w=74 rec=6 t=10.15s | ok w=51 rec=1 t=10.91s | ok w=52 rec=6 t=8.06s |
| schematicO3 | ok w=17 rec=0 t=0.22s | ok w=21 rec=0 t=0.72s | ok w=29 rec=0 t=0.36s | ok w=29 rec=0 t=6.04s | ok w=23 rec=1 t=3.19s | ok w=22 rec=0 t=1.13s | ok w=21 rec=0 t=3.09s | ok w=28 rec=1 t=2.40s | ok w=20 rec=0 t=3.42s | ok w=22 rec=3 t=3.97s |


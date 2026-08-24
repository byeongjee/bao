# Intermittent-power results (v2)

13 benchmarks x 4 algorithms x 10 traces (benchmarks/traces/1..10.csv replayed 10x faster than recorded), board cap 11.5 uF, 560 Ohm series resistor, wait threshold 3.0 V, halt-mode wait, cpu 16 MHz, no device-debug.

Cell: `ok` = completed (stop pulse + __nvm_done) and return code equals the uninstrumented baseline (`WRONG` otherwise); `incomplete` = did not finish within the trace; w = waits (cnt_wait: boundary/boot found VCC below 3.0 V and slept); rec = recovery boots (cnt_recovery, real brownouts); t = execution time incl. wait/outage time (Saleae start->stop pulse).

## Totals per algorithm

| algo | runs | complete | correct (of complete) | incomplete | mean waits | mean rec | mean t (s) |
|---|---|---|---|---|---|---|---|
| milp | 130 | 130 | 130 | 0 | 23.6 | 0.9 | 3.16 |
| rockclimb | 130 | 130 | 130 | 0 | 315.7 | 12.1 | 44.41 |
| schematic | 130 | 130 | 130 | 0 | 126.0 | 5.2 | 17.88 |
| schematicO3 | 130 | 130 | 130 | 0 | 38.3 | 1.3 | 5.16 |

## Per benchmark: complete / correct / total waits / total rec / mean t (s)

| benchmark (baseline result) | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes (107) | 10/10 / 10/10 / 264 / 13 / 3.69 | 10/10 / 10/10 / 2667 / 101 / 37.11 | 10/10 / 10/10 / 5922 / 268 / 89.23 | 10/10 / 10/10 / 687 / 23 / 9.10 |
| crc (20431) | 10/10 / 10/10 / 72 / 1 / 0.78 | 10/10 / 10/10 / 952 / 34 / 12.95 | 10/10 / 10/10 / 1863 / 68 / 25.19 | 10/10 / 10/10 / 82 / 1 / 0.79 |
| rsa (32) | 10/10 / 10/10 / 170 / 4 / 1.77 | 10/10 / 10/10 / 6141 / 239 / 87.95 | 10/10 / 10/10 / 620 / 18 / 7.63 | 10/10 / 10/10 / 274 / 7 / 3.39 |
| dijkstra (3788) | 10/10 / 10/10 / 98 / 1 / 0.88 | 10/10 / 10/10 / 7625 / 301 / 108.80 | 10/10 / 10/10 / 686 / 23 / 9.08 | 10/10 / 10/10 / 118 / 0 / 1.03 |
| qsort (9987) | 10/10 / 10/10 / 121 / 0 / 1.09 | 10/10 / 10/10 / 1587 / 58 / 21.90 | 10/10 / 10/10 / 237 / 5 / 2.50 | 10/10 / 10/10 / 210 / 2 / 1.86 |
| activity_recognition (64) | 10/10 / 10/10 / 129 / 0 / 1.30 | 10/10 / 10/10 / 726 / 26 / 9.67 | 10/10 / 10/10 / 582 / 18 / 7.28 | 10/10 / 10/10 / 174 / 2 / 1.77 |
| bitcount (14121) | 10/10 / 10/10 / 91 / 0 / 0.84 | 10/10 / 10/10 / 4178 / 159 / 59.04 | 10/10 / 10/10 / 444 / 12 / 5.45 | 10/10 / 10/10 / 103 / 0 / 0.99 |
| chacha20 (79) | 10/10 / 10/10 / 116 / 2 / 1.35 | 10/10 / 10/10 / 635 / 19 / 8.08 | 10/10 / 10/10 / 360 / 16 / 4.90 | 10/10 / 10/10 / 255 / 8 / 3.39 |
| sensor_fusion (613) | 10/10 / 10/10 / 113 / 0 / 1.00 | 10/10 / 10/10 / 126 / 0 / 1.12 | 10/10 / 10/10 / 166 / 1 / 1.62 | 10/10 / 10/10 / 168 / 2 / 1.75 |
| poly1305 (68) | 10/10 / 10/10 / 1362 / 78 / 22.47 | 10/10 / 10/10 / 4423 / 169 / 61.44 | 10/10 / 10/10 / 2967 / 147 / 44.49 | 10/10 / 10/10 / 2113 / 99 / 33.08 |
| cuckoo_filter (4048) | 10/10 / 10/10 / 145 / 1 / 1.40 | 10/10 / 10/10 / 1888 / 71 / 26.07 | 10/10 / 10/10 / 1358 / 60 / 19.55 | 10/10 / 10/10 / 221 / 8 / 2.55 |
| sha256_fixed (131) | 10/10 / 10/10 / 186 / 11 / 2.67 | 10/10 / 10/10 / 1093 / 35 / 13.93 | 10/10 / 10/10 / 471 / 19 / 6.19 | 10/10 / 10/10 / 348 / 16 / 4.86 |
| stringsearch (20) | 10/10 / 10/10 / 197 / 2 / 1.85 | 10/10 / 10/10 / 9005 / 355 / 129.31 | 10/10 / 10/10 / 700 / 27 / 9.38 | 10/10 / 10/10 / 231 / 5 / 2.45 |

## Per trace: waits by benchmark x algorithm (`inc` = incomplete)

### trace 1

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 26 | 181 | 460 | 56 |
| crc | 7 | 65 | 135 | 8 |
| rsa | 16 | 413 | 49 | 20 |
| dijkstra | 9 | 506 | 53 | 10 |
| qsort | 11 | 106 | 17 | 15 |
| activity_recognition | 11 | 49 | 45 | 14 |
| bitcount | 8 | 280 | 33 | 9 |
| chacha20 | 11 | 46 | 34 | 22 |
| sensor_fusion | 9 | 10 | 14 | 12 |
| poly1305 | 132 | 318 | 273 | 185 |
| cuckoo_filter | 12 | 132 | 115 | 18 |
| sha256_fixed | 18 | 74 | 37 | 35 |
| stringsearch | 15 | 601 | 57 | 17 |

### trace 2

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 25 | 246 | 562 | 63 |
| crc | 7 | 85 | 178 | 8 |
| rsa | 15 | 551 | 61 | 24 |
| dijkstra | 10 | 727 | 67 | 11 |
| qsort | 13 | 154 | 21 | 21 |
| activity_recognition | 13 | 69 | 57 | 16 |
| bitcount | 10 | 393 | 39 | 11 |
| chacha20 | 12 | 60 | 34 | 24 |
| sensor_fusion | 12 | 12 | 16 | 16 |
| poly1305 | 131 | 422 | 285 | 190 |
| cuckoo_filter | 14 | 178 | 124 | 20 |
| sha256_fixed | 18 | 104 | 48 | 33 |
| stringsearch | 18 | 813 | 66 | 22 |

### trace 3

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 25 | 417 | 786 | 93 |
| crc | 7 | 152 | 282 | 8 |
| rsa | 19 | 982 | 88 | 38 |
| dijkstra | 11 | 1208 | 98 | 14 |
| qsort | 11 | 250 | 30 | 25 |
| activity_recognition | 12 | 113 | 87 | 21 |
| bitcount | 7 | 664 | 64 | 9 |
| chacha20 | 11 | 98 | 37 | 28 |
| sensor_fusion | 13 | 14 | 20 | 21 |
| poly1305 | 129 | 662 | 329 | 243 |
| cuckoo_filter | 17 | 303 | 174 | 26 |
| sha256_fixed | 17 | 178 | 63 | 33 |
| stringsearch | 24 | 1434 | 94 | 28 |

### trace 4

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 28 | 336 | 625 | 70 |
| crc | 8 | 118 | 213 | 9 |
| rsa | 19 | 781 | 67 | 32 |
| dijkstra | 10 | 946 | 72 | 12 |
| qsort | 13 | 206 | 31 | 26 |
| activity_recognition | 12 | 85 | 61 | 17 |
| bitcount | 9 | 538 | 50 | 10 |
| chacha20 | 12 | 71 | 36 | 25 |
| sensor_fusion | 11 | 14 | 17 | 20 |
| poly1305 | 135 | 522 | 311 | 197 |
| cuckoo_filter | 17 | 230 | 141 | 27 |
| sha256_fixed | 19 | 135 | 48 | 34 |
| stringsearch | 22 | 1158 | 77 | 29 |

### trace 5

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 27 | 235 | 602 | 68 |
| crc | 7 | 87 | 177 | 8 |
| rsa | 18 | 560 | 60 | 27 |
| dijkstra | 10 | 696 | 66 | 12 |
| qsort | 12 | 143 | 24 | 21 |
| activity_recognition | 13 | 67 | 55 | 19 |
| bitcount | 9 | 373 | 43 | 10 |
| chacha20 | 11 | 57 | 37 | 26 |
| sensor_fusion | 11 | 13 | 16 | 16 |
| poly1305 | 137 | 405 | 293 | 215 |
| cuckoo_filter | 12 | 171 | 133 | 22 |
| sha256_fixed | 18 | 95 | 50 | 35 |
| stringsearch | 20 | 816 | 70 | 23 |

### trace 6

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 25 | 233 | 553 | 64 |
| crc | 7 | 81 | 161 | 8 |
| rsa | 15 | 525 | 54 | 23 |
| dijkstra | 9 | 652 | 59 | 12 |
| qsort | 12 | 133 | 23 | 22 |
| activity_recognition | 14 | 67 | 50 | 19 |
| bitcount | 10 | 352 | 38 | 11 |
| chacha20 | 11 | 54 | 33 | 24 |
| sensor_fusion | 12 | 14 | 16 | 17 |
| poly1305 | 130 | 387 | 278 | 204 |
| cuckoo_filter | 14 | 159 | 128 | 21 |
| sha256_fixed | 17 | 93 | 39 | 33 |
| stringsearch | 20 | 758 | 64 | 22 |

### trace 7

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 25 | 236 | 534 | 63 |
| crc | 7 | 85 | 170 | 8 |
| rsa | 16 | 542 | 58 | 26 |
| dijkstra | 10 | 665 | 62 | 13 |
| qsort | 13 | 136 | 23 | 20 |
| activity_recognition | 14 | 64 | 53 | 18 |
| bitcount | 9 | 366 | 42 | 11 |
| chacha20 | 11 | 58 | 35 | 24 |
| sensor_fusion | 12 | 13 | 18 | 18 |
| poly1305 | 128 | 395 | 271 | 215 |
| cuckoo_filter | 15 | 162 | 127 | 21 |
| sha256_fixed | 17 | 93 | 42 | 33 |
| stringsearch | 21 | 809 | 62 | 20 |

### trace 8

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 29 | 339 | 726 | 84 |
| crc | 8 | 124 | 232 | 9 |
| rsa | 19 | 797 | 75 | 34 |
| dijkstra | 11 | 990 | 86 | 14 |
| qsort | 15 | 199 | 27 | 26 |
| activity_recognition | 17 | 94 | 74 | 22 |
| bitcount | 12 | 541 | 56 | 14 |
| chacha20 | 13 | 81 | 37 | 31 |
| sensor_fusion | 15 | 16 | 19 | 21 |
| poly1305 | 148 | 560 | 319 | 251 |
| cuckoo_filter | 17 | 246 | 158 | 24 |
| sha256_fixed | 23 | 140 | 54 | 37 |
| stringsearch | 24 | 1176 | 78 | 28 |

### trace 9

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 26 | 217 | 528 | 60 |
| crc | 7 | 78 | 154 | 8 |
| rsa | 18 | 491 | 53 | 25 |
| dijkstra | 9 | 612 | 60 | 11 |
| qsort | 11 | 130 | 19 | 19 |
| activity_recognition | 12 | 60 | 50 | 15 |
| bitcount | 9 | 333 | 38 | 9 |
| chacha20 | 13 | 58 | 38 | 25 |
| sensor_fusion | 9 | 10 | 16 | 15 |
| poly1305 | 137 | 367 | 300 | 198 |
| cuckoo_filter | 15 | 154 | 123 | 20 |
| sha256_fixed | 19 | 93 | 46 | 36 |
| stringsearch | 18 | 709 | 64 | 20 |

### trace 10

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 28 | 227 | 546 | 66 |
| crc | 7 | 77 | 161 | 8 |
| rsa | 15 | 499 | 55 | 25 |
| dijkstra | 9 | 623 | 63 | 9 |
| qsort | 10 | 130 | 22 | 15 |
| activity_recognition | 11 | 58 | 50 | 13 |
| bitcount | 8 | 338 | 41 | 9 |
| chacha20 | 11 | 52 | 39 | 26 |
| sensor_fusion | 9 | 10 | 14 | 12 |
| poly1305 | 155 | 385 | 308 | 215 |
| cuckoo_filter | 12 | 153 | 135 | 22 |
| sha256_fixed | 20 | 88 | 44 | 39 |
| stringsearch | 15 | 731 | 68 | 22 |

## Per benchmark x algorithm x trace

### aes (baseline result 107)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=26 rec=1 t=0.97s | ok w=25 rec=0 t=0.89s | ok w=25 rec=0 t=0.56s | ok w=28 rec=3 t=6.09s | ok w=27 rec=2 t=6.22s | ok w=25 rec=0 t=1.24s | ok w=25 rec=0 t=5.51s | ok w=29 rec=3 t=5.46s | ok w=26 rec=1 t=5.88s | ok w=28 rec=3 t=4.10s |
| rockclimb | ok w=181 rec=5 t=13.47s | ok w=246 rec=4 t=16.50s | ok w=417 rec=4 t=15.80s | ok w=336 rec=0 t=68.91s | ok w=235 rec=15 t=48.38s | ok w=233 rec=4 t=21.29s | ok w=236 rec=0 t=36.84s | ok w=339 rec=33 t=51.75s | ok w=217 rec=0 t=50.91s | ok w=227 rec=36 t=47.29s |
| schematic | ok w=460 rec=14 t=35.36s | ok w=562 rec=11 t=43.74s | ok w=786 rec=8 t=32.59s | ok w=625 rec=0 t=152.78s | ok w=602 rec=39 t=122.74s | ok w=553 rec=10 t=52.71s | ok w=534 rec=0 t=88.86s | ok w=726 rec=81 t=125.66s | ok w=528 rec=18 t=123.41s | ok w=546 rec=87 t=114.44s |
| schematicO3 | ok w=56 rec=1 t=3.40s | ok w=63 rec=1 t=4.46s | ok w=93 rec=0 t=1.40s | ok w=70 rec=0 t=16.50s | ok w=68 rec=4 t=12.45s | ok w=64 rec=1 t=5.83s | ok w=63 rec=0 t=10.00s | ok w=84 rec=7 t=11.62s | ok w=60 rec=0 t=13.40s | ok w=66 rec=9 t=11.91s |

### crc (baseline result 20431)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=7 rec=0 t=0.10s | ok w=7 rec=0 t=0.54s | ok w=7 rec=0 t=0.12s | ok w=8 rec=1 t=0.84s | ok w=7 rec=0 t=1.84s | ok w=7 rec=0 t=0.67s | ok w=7 rec=0 t=1.87s | ok w=8 rec=0 t=0.85s | ok w=7 rec=0 t=0.89s | ok w=7 rec=0 t=0.10s |
| rockclimb | ok w=65 rec=2 t=5.08s | ok w=85 rec=1 t=4.79s | ok w=152 rec=1 t=4.63s | ok w=118 rec=0 t=24.37s | ok w=87 rec=5 t=17.37s | ok w=81 rec=1 t=6.33s | ok w=85 rec=0 t=13.53s | ok w=124 rec=12 t=19.25s | ok w=78 rec=0 t=18.38s | ok w=77 rec=12 t=15.82s |
| schematic | ok w=135 rec=3 t=8.59s | ok w=178 rec=3 t=12.18s | ok w=282 rec=2 t=9.06s | ok w=213 rec=0 t=45.31s | ok w=177 rec=11 t=34.14s | ok w=161 rec=3 t=14.09s | ok w=170 rec=0 t=26.37s | ok w=232 rec=22 t=34.72s | ok w=154 rec=0 t=35.85s | ok w=161 rec=24 t=31.61s |
| schematicO3 | ok w=8 rec=0 t=0.11s | ok w=8 rec=0 t=0.54s | ok w=8 rec=0 t=0.14s | ok w=9 rec=1 t=0.86s | ok w=8 rec=0 t=1.84s | ok w=8 rec=0 t=0.69s | ok w=8 rec=0 t=1.87s | ok w=9 rec=0 t=0.87s | ok w=8 rec=0 t=0.92s | ok w=8 rec=0 t=0.11s |

### rsa (baseline result 32)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=16 rec=0 t=0.21s | ok w=15 rec=0 t=0.66s | ok w=19 rec=0 t=0.26s | ok w=19 rec=1 t=3.44s | ok w=18 rec=1 t=3.11s | ok w=15 rec=0 t=1.09s | ok w=16 rec=0 t=3.02s | ok w=19 rec=1 t=2.32s | ok w=18 rec=1 t=3.38s | ok w=15 rec=0 t=0.20s |
| rockclimb | ok w=413 rec=13 t=32.88s | ok w=551 rec=10 t=39.96s | ok w=982 rec=9 t=36.54s | ok w=781 rec=0 t=168.53s | ok w=560 rec=36 t=113.46s | ok w=525 rec=9 t=48.05s | ok w=542 rec=0 t=86.99s | ok w=797 rec=81 t=125.65s | ok w=491 rec=0 t=120.85s | ok w=499 rec=81 t=106.56s |
| schematic | ok w=49 rec=1 t=2.70s | ok w=61 rec=1 t=4.35s | ok w=88 rec=0 t=1.31s | ok w=67 rec=0 t=13.94s | ok w=60 rec=3 t=11.17s | ok w=54 rec=1 t=4.69s | ok w=58 rec=0 t=9.04s | ok w=75 rec=6 t=10.15s | ok w=53 rec=0 t=10.92s | ok w=55 rec=6 t=8.08s |
| schematicO3 | ok w=20 rec=0 t=0.86s | ok w=24 rec=0 t=0.76s | ok w=38 rec=0 t=0.55s | ok w=32 rec=0 t=6.07s | ok w=27 rec=1 t=4.95s | ok w=23 rec=0 t=1.17s | ok w=26 rec=0 t=4.44s | ok w=34 rec=3 t=5.39s | ok w=25 rec=0 t=5.74s | ok w=25 rec=3 t=4.01s |

### dijkstra (baseline result 3788)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=9 rec=0 t=0.11s | ok w=10 rec=0 t=0.55s | ok w=11 rec=0 t=0.15s | ok w=10 rec=1 t=1.46s | ok w=10 rec=0 t=1.85s | ok w=9 rec=0 t=0.70s | ok w=10 rec=0 t=2.02s | ok w=11 rec=0 t=0.87s | ok w=9 rec=0 t=0.93s | ok w=9 rec=0 t=0.11s |
| rockclimb | ok w=506 rec=16 t=40.49s | ok w=727 rec=13 t=52.61s | ok w=1208 rec=11 t=44.73s | ok w=946 rec=0 t=199.93s | ok w=696 rec=45 t=139.54s | ok w=652 rec=12 t=61.45s | ok w=665 rec=0 t=106.46s | ok w=990 rec=102 t=157.86s | ok w=612 rec=0 t=150.85s | ok w=623 rec=102 t=134.11s |
| schematic | ok w=53 rec=1 t=3.36s | ok w=67 rec=1 t=4.46s | ok w=98 rec=0 t=1.39s | ok w=72 rec=0 t=16.53s | ok w=66 rec=4 t=12.42s | ok w=59 rec=1 t=5.79s | ok w=62 rec=0 t=9.98s | ok w=86 rec=7 t=11.61s | ok w=60 rec=0 t=13.37s | ok w=63 rec=9 t=11.88s |
| schematicO3 | ok w=10 rec=0 t=0.13s | ok w=11 rec=0 t=0.57s | ok w=14 rec=0 t=0.18s | ok w=12 rec=0 t=2.62s | ok w=12 rec=0 t=1.87s | ok w=12 rec=0 t=0.88s | ok w=13 rec=0 t=2.05s | ok w=14 rec=0 t=0.89s | ok w=11 rec=0 t=0.94s | ok w=9 rec=0 t=0.12s |

### qsort (baseline result 9987)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=11 rec=0 t=0.14s | ok w=13 rec=0 t=0.62s | ok w=11 rec=0 t=0.15s | ok w=13 rec=0 t=3.39s | ok w=12 rec=0 t=1.86s | ok w=12 rec=0 t=0.72s | ok w=13 rec=0 t=2.05s | ok w=15 rec=0 t=0.91s | ok w=11 rec=0 t=0.95s | ok w=10 rec=0 t=0.13s |
| rockclimb | ok w=106 rec=3 t=7.72s | ok w=154 rec=2 t=9.50s | ok w=250 rec=2 t=8.52s | ok w=206 rec=0 t=42.72s | ok w=143 rec=9 t=27.97s | ok w=133 rec=2 t=11.50s | ok w=136 rec=0 t=22.51s | ok w=199 rec=19 t=30.08s | ok w=130 rec=0 t=30.86s | ok w=130 rec=21 t=27.61s |
| schematic | ok w=17 rec=0 t=0.22s | ok w=21 rec=0 t=0.73s | ok w=30 rec=0 t=0.36s | ok w=31 rec=0 t=6.03s | ok w=24 rec=1 t=3.20s | ok w=23 rec=0 t=1.14s | ok w=23 rec=0 t=3.49s | ok w=27 rec=1 t=2.41s | ok w=19 rec=0 t=3.41s | ok w=22 rec=3 t=3.97s |
| schematicO3 | ok w=15 rec=0 t=0.19s | ok w=21 rec=0 t=0.72s | ok w=25 rec=0 t=0.30s | ok w=26 rec=0 t=4.09s | ok w=21 rec=1 t=3.16s | ok w=22 rec=0 t=1.10s | ok w=20 rec=0 t=3.07s | ok w=26 rec=1 t=2.38s | ok w=19 rec=0 t=3.40s | ok w=15 rec=0 t=0.19s |

### activity_recognition (baseline result 64)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=11 rec=0 t=0.14s | ok w=13 rec=0 t=0.62s | ok w=12 rec=0 t=0.17s | ok w=12 rec=0 t=3.39s | ok w=13 rec=0 t=1.89s | ok w=14 rec=0 t=1.05s | ok w=14 rec=0 t=2.07s | ok w=17 rec=0 t=0.92s | ok w=12 rec=0 t=2.62s | ok w=11 rec=0 t=0.14s |
| rockclimb | ok w=49 rec=1 t=3.36s | ok w=69 rec=1 t=4.49s | ok w=113 rec=1 t=4.06s | ok w=85 rec=0 t=16.57s | ok w=67 rec=4 t=12.48s | ok w=67 rec=1 t=5.84s | ok w=64 rec=0 t=10.03s | ok w=94 rec=9 t=14.62s | ok w=60 rec=0 t=13.39s | ok w=58 rec=9 t=11.86s |
| schematic | ok w=45 rec=1 t=2.65s | ok w=57 rec=1 t=3.92s | ok w=87 rec=0 t=1.28s | ok w=61 rec=0 t=13.89s | ok w=55 rec=3 t=9.39s | ok w=50 rec=1 t=3.79s | ok w=53 rec=0 t=8.82s | ok w=74 rec=6 t=10.12s | ok w=50 rec=0 t=10.89s | ok w=50 rec=6 t=8.03s |
| schematicO3 | ok w=14 rec=0 t=0.18s | ok w=16 rec=0 t=0.64s | ok w=21 rec=0 t=0.26s | ok w=17 rec=0 t=3.45s | ok w=19 rec=1 t=3.13s | ok w=19 rec=0 t=1.09s | ok w=18 rec=0 t=3.05s | ok w=22 rec=1 t=2.33s | ok w=15 rec=0 t=3.36s | ok w=13 rec=0 t=0.17s |

### bitcount (baseline result 14121)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=8 rec=0 t=0.11s | ok w=10 rec=0 t=0.54s | ok w=7 rec=0 t=0.12s | ok w=9 rec=0 t=1.46s | ok w=9 rec=0 t=1.84s | ok w=10 rec=0 t=0.70s | ok w=9 rec=0 t=1.88s | ok w=12 rec=0 t=0.87s | ok w=9 rec=0 t=0.81s | ok w=8 rec=0 t=0.11s |
| rockclimb | ok w=280 rec=8 t=21.18s | ok w=393 rec=7 t=27.85s | ok w=664 rec=6 t=24.50s | ok w=538 rec=0 t=113.46s | ok w=373 rec=24 t=76.29s | ok w=352 rec=6 t=32.20s | ok w=366 rec=0 t=58.76s | ok w=541 rec=54 t=84.08s | ok w=333 rec=0 t=80.93s | ok w=338 rec=54 t=71.11s |
| schematic | ok w=33 rec=0 t=1.02s | ok w=39 rec=0 t=1.67s | ok w=64 rec=0 t=1.05s | ok w=50 rec=0 t=11.27s | ok w=43 rec=2 t=8.06s | ok w=38 rec=0 t=1.63s | ok w=42 rec=0 t=6.52s | ok w=56 rec=4 t=7.00s | ok w=38 rec=0 t=8.37s | ok w=41 rec=6 t=7.91s |
| schematicO3 | ok w=9 rec=0 t=0.12s | ok w=11 rec=0 t=0.54s | ok w=9 rec=0 t=0.14s | ok w=10 rec=0 t=2.62s | ok w=10 rec=0 t=1.85s | ok w=11 rec=0 t=0.71s | ok w=11 rec=0 t=2.03s | ok w=14 rec=0 t=0.89s | ok w=9 rec=0 t=0.92s | ok w=9 rec=0 t=0.12s |

### chacha20 (baseline result 79)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=11 rec=0 t=0.16s | ok w=12 rec=0 t=0.62s | ok w=11 rec=0 t=0.21s | ok w=12 rec=1 t=3.27s | ok w=11 rec=0 t=1.89s | ok w=11 rec=0 t=1.04s | ok w=11 rec=0 t=2.05s | ok w=13 rec=0 t=0.91s | ok w=13 rec=1 t=3.21s | ok w=11 rec=0 t=0.16s |
| rockclimb | ok w=46 rec=1 t=2.70s | ok w=60 rec=1 t=4.31s | ok w=98 rec=0 t=1.40s | ok w=71 rec=0 t=14.55s | ok w=57 rec=3 t=11.17s | ok w=54 rec=1 t=4.69s | ok w=58 rec=0 t=9.03s | ok w=81 rec=7 t=11.56s | ok w=58 rec=0 t=13.33s | ok w=52 rec=6 t=8.06s |
| schematic | ok w=34 rec=1 t=2.52s | ok w=34 rec=0 t=1.66s | ok w=37 rec=0 t=0.76s | ok w=36 rec=1 t=8.66s | ok w=37 rec=2 t=6.28s | ok w=33 rec=0 t=1.62s | ok w=35 rec=0 t=5.69s | ok w=37 rec=3 t=5.53s | ok w=38 rec=3 t=8.35s | ok w=39 rec=6 t=7.91s |
| schematicO3 | ok w=22 rec=0 t=0.89s | ok w=24 rec=0 t=0.81s | ok w=28 rec=0 t=0.37s | ok w=25 rec=0 t=6.04s | ok w=26 rec=1 t=4.97s | ok w=24 rec=0 t=1.18s | ok w=24 rec=0 t=4.43s | ok w=31 rec=3 t=5.38s | ok w=25 rec=1 t=5.83s | ok w=26 rec=3 t=4.02s |

### sensor_fusion (baseline result 613)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=9 rec=0 t=0.11s | ok w=12 rec=0 t=0.55s | ok w=13 rec=0 t=0.17s | ok w=11 rec=0 t=2.62s | ok w=11 rec=0 t=1.86s | ok w=12 rec=0 t=0.72s | ok w=12 rec=0 t=2.03s | ok w=15 rec=0 t=0.89s | ok w=9 rec=0 t=0.92s | ok w=9 rec=0 t=0.11s |
| rockclimb | ok w=10 rec=0 t=0.13s | ok w=12 rec=0 t=0.57s | ok w=14 rec=0 t=0.18s | ok w=14 rec=0 t=3.40s | ok w=13 rec=0 t=1.88s | ok w=14 rec=0 t=1.04s | ok w=13 rec=0 t=2.06s | ok w=16 rec=0 t=0.90s | ok w=10 rec=0 t=0.93s | ok w=10 rec=0 t=0.13s |
| schematic | ok w=14 rec=0 t=0.18s | ok w=16 rec=0 t=0.65s | ok w=20 rec=0 t=0.25s | ok w=17 rec=0 t=3.43s | ok w=16 rec=1 t=3.11s | ok w=16 rec=0 t=1.08s | ok w=18 rec=0 t=3.03s | ok w=19 rec=0 t=0.94s | ok w=16 rec=0 t=3.36s | ok w=14 rec=0 t=0.18s |
| schematicO3 | ok w=12 rec=0 t=0.15s | ok w=16 rec=0 t=0.63s | ok w=21 rec=0 t=0.25s | ok w=20 rec=0 t=3.43s | ok w=16 rec=1 t=3.10s | ok w=17 rec=0 t=1.07s | ok w=18 rec=0 t=3.04s | ok w=21 rec=1 t=2.33s | ok w=15 rec=0 t=3.35s | ok w=12 rec=0 t=0.15s |

### poly1305 (baseline result 68)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=132 rec=4 t=10.19s | ok w=131 rec=2 t=9.70s | ok w=129 rec=1 t=5.16s | ok w=135 rec=7 t=34.91s | ok w=137 rec=9 t=29.74s | ok w=130 rec=2 t=12.04s | ok w=128 rec=0 t=22.96s | ok w=148 rec=18 t=28.63s | ok w=137 rec=8 t=35.85s | ok w=155 rec=27 t=35.48s |
| rockclimb | ok w=318 rec=9 t=22.90s | ok w=422 rec=7 t=28.33s | ok w=662 rec=6 t=24.44s | ok w=522 rec=0 t=113.49s | ok w=405 rec=25 t=79.38s | ok w=387 rec=7 t=34.72s | ok w=395 rec=0 t=60.79s | ok w=560 rec=55 t=85.56s | ok w=367 rec=0 t=85.90s | ok w=385 rec=60 t=78.85s |
| schematic | ok w=273 rec=8 t=20.24s | ok w=285 rec=5 t=20.50s | ok w=329 rec=3 t=13.02s | ok w=311 rec=4 t=74.16s | ok w=293 rec=19 t=58.98s | ok w=278 rec=6 t=26.51s | ok w=271 rec=0 t=43.63s | ok w=319 rec=36 t=56.33s | ok w=300 rec=18 t=68.34s | ok w=308 rec=48 t=63.14s |
| schematicO3 | ok w=185 rec=5 t=13.51s | ok w=190 rec=3 t=13.87s | ok w=243 rec=2 t=9.23s | ok w=197 rec=0 t=53.23s | ok w=215 rec=14 t=45.26s | ok w=204 rec=5 t=20.09s | ok w=215 rec=0 t=36.68s | ok w=251 rec=30 t=47.01s | ok w=198 rec=7 t=48.37s | ok w=215 rec=33 t=43.54s |

### cuckoo_filter (baseline result 4048)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=12 rec=0 t=0.16s | ok w=14 rec=0 t=0.64s | ok w=17 rec=0 t=0.24s | ok w=17 rec=0 t=3.43s | ok w=12 rec=0 t=1.90s | ok w=14 rec=0 t=1.05s | ok w=15 rec=0 t=2.08s | ok w=17 rec=0 t=0.94s | ok w=15 rec=1 t=3.35s | ok w=12 rec=0 t=0.16s |
| rockclimb | ok w=132 rec=4 t=10.11s | ok w=178 rec=3 t=12.40s | ok w=303 rec=2 t=9.27s | ok w=230 rec=0 t=47.97s | ok w=171 rec=11 t=34.17s | ok w=159 rec=3 t=14.96s | ok w=162 rec=0 t=26.41s | ok w=246 rec=24 t=37.83s | ok w=154 rec=0 t=35.91s | ok w=153 rec=24 t=31.64s |
| schematic | ok w=115 rec=4 t=7.74s | ok w=124 rec=2 t=8.64s | ok w=174 rec=1 t=5.27s | ok w=141 rec=2 t=34.87s | ok w=133 rec=8 t=26.63s | ok w=128 rec=2 t=11.42s | ok w=127 rec=0 t=19.47s | ok w=158 rec=16 t=25.50s | ok w=123 rec=4 t=28.39s | ok w=135 rec=21 t=27.58s |
| schematicO3 | ok w=18 rec=1 t=0.84s | ok w=20 rec=0 t=0.75s | ok w=26 rec=0 t=0.36s | ok w=27 rec=2 t=5.90s | ok w=22 rec=1 t=3.19s | ok w=21 rec=0 t=1.16s | ok w=21 rec=0 t=3.48s | ok w=24 rec=1 t=2.40s | ok w=20 rec=0 t=3.42s | ok w=22 rec=3 t=3.98s |

### sha256_fixed (baseline result 131)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=18 rec=1 t=0.88s | ok w=18 rec=0 t=0.75s | ok w=17 rec=0 t=0.33s | ok w=19 rec=2 t=5.24s | ok w=18 rec=1 t=3.18s | ok w=17 rec=0 t=1.14s | ok w=17 rec=0 t=3.08s | ok w=23 rec=3 t=4.63s | ok w=19 rec=1 t=3.43s | ok w=20 rec=3 t=3.99s |
| rockclimb | ok w=74 rec=2 t=5.14s | ok w=104 rec=1 t=5.77s | ok w=178 rec=1 t=5.01s | ok w=135 rec=0 t=27.00s | ok w=95 rec=6 t=18.65s | ok w=93 rec=1 t=6.75s | ok w=93 rec=0 t=14.86s | ok w=140 rec=12 t=19.38s | ok w=93 rec=0 t=20.85s | ok w=88 rec=12 t=15.91s |
| schematic | ok w=37 rec=1 t=2.60s | ok w=48 rec=0 t=2.07s | ok w=63 rec=0 t=1.12s | ok w=48 rec=0 t=11.31s | ok w=50 rec=3 t=9.32s | ok w=39 rec=0 t=1.68s | ok w=42 rec=0 t=6.57s | ok w=54 rec=6 t=9.28s | ok w=46 rec=3 t=10.00s | ok w=44 rec=6 t=7.98s |
| schematicO3 | ok w=35 rec=2 t=2.54s | ok w=33 rec=0 t=1.62s | ok w=33 rec=0 t=0.67s | ok w=34 rec=1 t=8.66s | ok w=35 rec=2 t=6.27s | ok w=33 rec=0 t=1.61s | ok w=33 rec=0 t=5.52s | ok w=37 rec=3 t=5.51s | ok w=36 rec=2 t=8.33s | ok w=39 rec=6 t=7.91s |

### stringsearch (baseline result 20)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=15 rec=0 t=0.19s | ok w=18 rec=0 t=0.70s | ok w=24 rec=0 t=0.30s | ok w=22 rec=0 t=4.09s | ok w=20 rec=1 t=3.16s | ok w=20 rec=0 t=1.11s | ok w=21 rec=0 t=3.05s | ok w=24 rec=1 t=2.36s | ok w=18 rec=0 t=3.39s | ok w=15 rec=0 t=0.20s |
| rockclimb | ok w=601 rec=19 t=48.00s | ok w=813 rec=15 t=59.36s | ok w=1434 rec=14 t=55.40s | ok w=1158 rec=0 t=243.03s | ok w=816 rec=53 t=164.33s | ok w=758 rec=14 t=72.89s | ok w=809 rec=0 t=130.82s | ok w=1176 rec=120 t=185.62s | ok w=709 rec=0 t=175.88s | ok w=731 rec=120 t=157.75s |
| schematic | ok w=57 rec=1 t=3.41s | ok w=66 rec=1 t=4.55s | ok w=94 rec=1 t=3.94s | ok w=77 rec=0 t=16.55s | ok w=70 rec=4 t=12.48s | ok w=64 rec=1 t=5.85s | ok w=62 rec=0 t=10.05s | ok w=78 rec=7 t=11.62s | ok w=64 rec=3 t=13.40s | ok w=68 rec=9 t=11.94s |
| schematicO3 | ok w=17 rec=0 t=0.22s | ok w=22 rec=0 t=0.73s | ok w=28 rec=0 t=0.35s | ok w=29 rec=0 t=6.04s | ok w=23 rec=1 t=3.18s | ok w=22 rec=0 t=1.14s | ok w=20 rec=0 t=3.07s | ok w=28 rec=1 t=2.40s | ok w=20 rec=0 t=3.42s | ok w=22 rec=3 t=3.96s |


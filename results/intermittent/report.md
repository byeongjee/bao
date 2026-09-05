# Intermittent-power results (v2)

13 benchmarks x 4 algorithms x 10 traces (benchmarks/traces/1..10.csv replayed 10x faster than recorded), board cap 11.5 uF, 560 Ohm series resistor, wait threshold 3.0 V, halt-mode wait, cpu 16 MHz, no device-debug.

Cell: `ok` = completed (stop pulse + __nvm_done) and return code equals the uninstrumented baseline (`WRONG` otherwise); `incomplete` = did not finish within the trace; w = waits (cnt_wait: boundary/boot found VCC below 3.0 V and slept); rec = recovery boots (cnt_recovery, real brownouts); t = execution time incl. wait/outage time (Saleae start->stop pulse).

## Totals per algorithm

| algo | runs | complete | correct (of complete) | incomplete | mean waits | mean rec | mean t (s) |
|---|---|---|---|---|---|---|---|
| milp | 130 | 130 | 130 | 0 | 23.9 | 0.9 | 3.13 |
| rockclimb | 130 | 130 | 130 | 0 | 312.2 | 11.9 | 43.57 |
| schematic | 130 | 130 | 130 | 0 | 125.2 | 5.1 | 17.58 |
| schematicO3 | 130 | 130 | 130 | 0 | 37.9 | 1.3 | 5.04 |

## Per benchmark: complete / correct / total waits / total rec / mean t (s)

| benchmark (baseline result) | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes (107) | 10/10 / 10/10 / 264 / 13 / 3.67 | 10/10 / 10/10 / 2643 / 97 / 35.60 | 10/10 / 10/10 / 5859 / 258 / 87.20 | 10/10 / 10/10 / 681 / 23 / 8.83 |
| crc (20431) | 10/10 / 10/10 / 72 / 1 / 0.59 | 10/10 / 10/10 / 951 / 34 / 12.95 | 10/10 / 10/10 / 1850 / 67 / 24.69 | 10/10 / 10/10 / 82 / 1 / 0.87 |
| rsa (32) | 10/10 / 10/10 / 220 / 7 / 2.45 | 10/10 / 10/10 / 5999 / 238 / 85.25 | 10/10 / 10/10 / 616 / 18 / 7.63 | 10/10 / 10/10 / 273 / 7 / 3.47 |
| dijkstra (3788) | 10/10 / 10/10 / 97 / 0 / 0.87 | 10/10 / 10/10 / 7558 / 296 / 106.57 | 10/10 / 10/10 / 684 / 23 / 8.85 | 10/10 / 10/10 / 117 / 0 / 1.02 |
| qsort (9987) | 10/10 / 10/10 / 121 / 0 / 1.09 | 10/10 / 10/10 / 1545 / 55 / 21.00 | 10/10 / 10/10 / 235 / 5 / 2.49 | 10/10 / 10/10 / 203 / 2 / 1.86 |
| activity_recognition (64) | 10/10 / 10/10 / 129 / 0 / 1.29 | 10/10 / 10/10 / 718 / 26 / 9.86 | 10/10 / 10/10 / 580 / 17 / 7.28 | 10/10 / 10/10 / 172 / 3 / 1.77 |
| bitcount (14121) | 10/10 / 10/10 / 90 / 0 / 0.81 | 10/10 / 10/10 / 4164 / 160 / 59.32 | 10/10 / 10/10 / 447 / 12 / 5.44 | 10/10 / 10/10 / 104 / 0 / 1.00 |
| chacha20 (79) | 10/10 / 10/10 / 115 / 2 / 1.30 | 10/10 / 10/10 / 626 / 18 / 7.64 | 10/10 / 10/10 / 356 / 15 / 4.89 | 10/10 / 10/10 / 252 / 8 / 3.38 |
| sensor_fusion (613) | 10/10 / 10/10 / 108 / 0 / 0.99 | 10/10 / 10/10 / 125 / 0 / 1.12 | 10/10 / 10/10 / 167 / 1 / 1.62 | 10/10 / 10/10 / 165 / 1 / 1.51 |
| poly1305 (68) | 10/10 / 10/10 / 1361 / 76 / 22.00 | 10/10 / 10/10 / 4385 / 165 / 60.79 | 10/10 / 10/10 / 2961 / 148 / 44.31 | 10/10 / 10/10 / 2081 / 97 / 31.95 |
| cuckoo_filter (4048) | 10/10 / 10/10 / 146 / 1 / 1.40 | 10/10 / 10/10 / 1898 / 71 / 25.94 | 10/10 / 10/10 / 1360 / 54 / 18.83 | 10/10 / 10/10 / 221 / 7 / 2.56 |
| sha256 (131) | 10/10 / 10/10 / 184 / 9 / 2.45 | 10/10 / 10/10 / 1073 / 35 / 13.64 | 10/10 / 10/10 / 464 / 18 / 6.21 | 10/10 / 10/10 / 348 / 15 / 4.85 |
| stringsearch (20) | 10/10 / 10/10 / 194 / 2 / 1.79 | 10/10 / 10/10 / 8905 / 346 / 126.74 | 10/10 / 10/10 / 691 / 23 / 9.09 | 10/10 / 10/10 / 232 / 5 / 2.45 |

## Per trace: waits by benchmark x algorithm (`inc` = incomplete)

### trace 1

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 26 | 179 | 459 | 56 |
| crc | 7 | 65 | 134 | 8 |
| rsa | 16 | 407 | 49 | 20 |
| dijkstra | 9 | 512 | 52 | 10 |
| qsort | 11 | 104 | 17 | 15 |
| activity_recognition | 11 | 49 | 44 | 14 |
| bitcount | 8 | 280 | 32 | 9 |
| chacha20 | 11 | 46 | 34 | 22 |
| sensor_fusion | 8 | 10 | 14 | 12 |
| poly1305 | 132 | 322 | 269 | 185 |
| cuckoo_filter | 13 | 130 | 117 | 18 |
| sha256 | 18 | 74 | 38 | 35 |
| stringsearch | 14 | 594 | 55 | 16 |

### trace 2

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 25 | 248 | 554 | 62 |
| crc | 7 | 85 | 177 | 8 |
| rsa | 19 | 546 | 59 | 24 |
| dijkstra | 10 | 676 | 60 | 11 |
| qsort | 11 | 136 | 20 | 19 |
| activity_recognition | 13 | 66 | 55 | 16 |
| bitcount | 8 | 381 | 40 | 11 |
| chacha20 | 12 | 61 | 34 | 24 |
| sensor_fusion | 12 | 12 | 16 | 16 |
| poly1305 | 131 | 407 | 281 | 191 |
| cuckoo_filter | 14 | 170 | 125 | 22 |
| sha256 | 18 | 99 | 45 | 33 |
| stringsearch | 19 | 804 | 68 | 21 |

### trace 3

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 25 | 409 | 768 | 92 |
| crc | 7 | 145 | 275 | 8 |
| rsa | 29 | 929 | 87 | 40 |
| dijkstra | 10 | 1210 | 97 | 14 |
| qsort | 12 | 236 | 28 | 25 |
| activity_recognition | 13 | 110 | 87 | 21 |
| bitcount | 7 | 663 | 63 | 9 |
| chacha20 | 11 | 96 | 37 | 28 |
| sensor_fusion | 13 | 14 | 20 | 20 |
| poly1305 | 129 | 651 | 335 | 241 |
| cuckoo_filter | 16 | 297 | 175 | 26 |
| sha256 | 18 | 172 | 61 | 33 |
| stringsearch | 24 | 1393 | 94 | 28 |

### trace 4

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 27 | 330 | 615 | 69 |
| crc | 8 | 118 | 209 | 9 |
| rsa | 29 | 747 | 66 | 33 |
| dijkstra | 9 | 931 | 75 | 12 |
| qsort | 14 | 205 | 32 | 23 |
| activity_recognition | 12 | 90 | 64 | 18 |
| bitcount | 8 | 532 | 53 | 10 |
| chacha20 | 12 | 72 | 35 | 25 |
| sensor_fusion | 11 | 14 | 18 | 22 |
| poly1305 | 133 | 518 | 298 | 189 |
| cuckoo_filter | 16 | 234 | 139 | 24 |
| sha256 | 19 | 130 | 49 | 35 |
| stringsearch | 21 | 1170 | 71 | 30 |

### trace 5

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 27 | 237 | 585 | 64 |
| crc | 7 | 88 | 177 | 8 |
| rsa | 23 | 537 | 61 | 26 |
| dijkstra | 10 | 699 | 66 | 12 |
| qsort | 12 | 138 | 23 | 22 |
| activity_recognition | 14 | 64 | 54 | 18 |
| bitcount | 9 | 388 | 43 | 11 |
| chacha20 | 11 | 57 | 36 | 27 |
| sensor_fusion | 11 | 12 | 17 | 16 |
| poly1305 | 138 | 407 | 291 | 214 |
| cuckoo_filter | 13 | 170 | 132 | 23 |
| sha256 | 18 | 98 | 46 | 35 |
| stringsearch | 20 | 789 | 67 | 22 |

### trace 6

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 25 | 228 | 542 | 67 |
| crc | 7 | 83 | 162 | 8 |
| rsa | 17 | 526 | 54 | 23 |
| dijkstra | 9 | 646 | 62 | 12 |
| qsort | 12 | 135 | 22 | 21 |
| activity_recognition | 13 | 64 | 51 | 18 |
| bitcount | 10 | 352 | 39 | 12 |
| chacha20 | 11 | 55 | 33 | 23 |
| sensor_fusion | 11 | 14 | 17 | 18 |
| poly1305 | 131 | 383 | 287 | 207 |
| cuckoo_filter | 13 | 164 | 128 | 19 |
| sha256 | 17 | 91 | 39 | 33 |
| stringsearch | 19 | 758 | 66 | 22 |

### trace 7

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 25 | 229 | 536 | 62 |
| crc | 7 | 86 | 166 | 8 |
| rsa | 20 | 514 | 57 | 24 |
| dijkstra | 10 | 640 | 61 | 13 |
| qsort | 12 | 133 | 24 | 20 |
| activity_recognition | 13 | 62 | 51 | 17 |
| bitcount | 9 | 354 | 41 | 10 |
| chacha20 | 11 | 55 | 34 | 23 |
| sensor_fusion | 11 | 13 | 17 | 15 |
| poly1305 | 128 | 385 | 276 | 191 |
| cuckoo_filter | 15 | 171 | 126 | 22 |
| sha256 | 17 | 93 | 45 | 33 |
| stringsearch | 20 | 791 | 61 | 23 |

### trace 8

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 29 | 342 | 725 | 81 |
| crc | 8 | 124 | 230 | 9 |
| rsa | 26 | 797 | 76 | 32 |
| dijkstra | 12 | 992 | 85 | 14 |
| qsort | 16 | 199 | 27 | 25 |
| activity_recognition | 17 | 92 | 73 | 21 |
| bitcount | 14 | 541 | 55 | 14 |
| chacha20 | 13 | 75 | 37 | 31 |
| sensor_fusion | 14 | 16 | 19 | 19 |
| poly1305 | 148 | 564 | 316 | 244 |
| cuckoo_filter | 18 | 248 | 158 | 25 |
| sha256 | 20 | 138 | 56 | 37 |
| stringsearch | 25 | 1153 | 81 | 27 |

### trace 9

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 27 | 221 | 526 | 62 |
| crc | 7 | 80 | 155 | 8 |
| rsa | 20 | 485 | 51 | 26 |
| dijkstra | 9 | 620 | 61 | 10 |
| qsort | 11 | 132 | 20 | 18 |
| activity_recognition | 12 | 60 | 52 | 16 |
| bitcount | 9 | 327 | 40 | 9 |
| chacha20 | 12 | 56 | 37 | 24 |
| sensor_fusion | 9 | 10 | 15 | 15 |
| poly1305 | 139 | 362 | 301 | 203 |
| cuckoo_filter | 15 | 155 | 126 | 20 |
| sha256 | 19 | 91 | 41 | 35 |
| stringsearch | 18 | 716 | 63 | 21 |

### trace 10

| benchmark | milp | rockclimb | schematic | schematicO3 |
|---|---|---|---|---|
| aes | 28 | 220 | 549 | 66 |
| crc | 7 | 77 | 165 | 8 |
| rsa | 21 | 511 | 56 | 25 |
| dijkstra | 9 | 632 | 65 | 9 |
| qsort | 10 | 127 | 22 | 15 |
| activity_recognition | 11 | 61 | 49 | 13 |
| bitcount | 8 | 346 | 41 | 9 |
| chacha20 | 11 | 53 | 39 | 25 |
| sensor_fusion | 8 | 10 | 14 | 12 |
| poly1305 | 152 | 386 | 307 | 216 |
| cuckoo_filter | 13 | 159 | 134 | 22 |
| sha256 | 20 | 87 | 44 | 39 |
| stringsearch | 14 | 737 | 65 | 22 |

## Per benchmark x algorithm x trace

### aes (baseline result 107)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=26 rec=1 t=0.96s | ok w=25 rec=0 t=0.89s | ok w=25 rec=0 t=0.56s | ok w=27 rec=2 t=6.09s | ok w=27 rec=2 t=6.20s | ok w=25 rec=0 t=1.25s | ok w=25 rec=0 t=5.33s | ok w=29 rec=3 t=5.43s | ok w=27 rec=2 t=5.87s | ok w=28 rec=3 t=4.08s |
| rockclimb | ok w=179 rec=5 t=13.44s | ok w=248 rec=4 t=16.38s | ok w=409 rec=3 t=13.21s | ok w=330 rec=0 t=63.69s | ok w=237 rec=15 t=46.58s | ok w=228 rec=4 t=20.18s | ok w=229 rec=0 t=36.43s | ok w=342 rec=33 t=51.73s | ok w=221 rec=0 t=50.86s | ok w=220 rec=33 t=43.50s |
| schematic | ok w=459 rec=14 t=35.29s | ok w=554 rec=10 t=40.87s | ok w=768 rec=8 t=32.15s | ok w=615 rec=0 t=146.72s | ok w=585 rec=38 t=117.82s | ok w=542 rec=11 t=51.11s | ok w=536 rec=0 t=88.63s | ok w=725 rec=81 t=125.57s | ok w=526 rec=12 t=123.37s | ok w=549 rec=84 t=110.47s |
| schematicO3 | ok w=56 rec=1 t=3.40s | ok w=62 rec=1 t=4.45s | ok w=92 rec=0 t=1.37s | ok w=69 rec=0 t=14.55s | ok w=64 rec=4 t=12.40s | ok w=67 rec=1 t=5.20s | ok w=62 rec=0 t=10.00s | ok w=81 rec=7 t=11.62s | ok w=62 rec=0 t=13.40s | ok w=66 rec=9 t=11.90s |

### crc (baseline result 20431)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=7 rec=0 t=0.10s | ok w=7 rec=0 t=0.42s | ok w=7 rec=0 t=0.12s | ok w=8 rec=1 t=0.84s | ok w=7 rec=0 t=0.09s | ok w=7 rec=0 t=0.67s | ok w=7 rec=0 t=1.86s | ok w=8 rec=0 t=0.85s | ok w=7 rec=0 t=0.90s | ok w=7 rec=0 t=0.10s |
| rockclimb | ok w=65 rec=2 t=5.05s | ok w=85 rec=1 t=4.80s | ok w=145 rec=1 t=4.58s | ok w=118 rec=0 t=22.42s | ok w=88 rec=5 t=17.35s | ok w=83 rec=1 t=6.33s | ok w=86 rec=0 t=15.54s | ok w=124 rec=12 t=19.25s | ok w=80 rec=0 t=18.34s | ok w=77 rec=12 t=15.80s |
| schematic | ok w=134 rec=3 t=8.57s | ok w=177 rec=3 t=12.18s | ok w=275 rec=2 t=8.98s | ok w=209 rec=0 t=42.75s | ok w=177 rec=11 t=34.10s | ok w=162 rec=2 t=12.02s | ok w=166 rec=0 t=26.21s | ok w=230 rec=22 t=34.70s | ok w=155 rec=0 t=35.84s | ok w=165 rec=24 t=31.59s |
| schematicO3 | ok w=8 rec=0 t=0.11s | ok w=8 rec=0 t=0.54s | ok w=8 rec=0 t=0.14s | ok w=9 rec=1 t=1.45s | ok w=8 rec=0 t=1.83s | ok w=8 rec=0 t=0.70s | ok w=8 rec=0 t=2.03s | ok w=9 rec=0 t=0.87s | ok w=8 rec=0 t=0.91s | ok w=8 rec=0 t=0.11s |

### rsa (baseline result 32)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=16 rec=0 t=0.22s | ok w=19 rec=0 t=0.74s | ok w=29 rec=0 t=0.36s | ok w=29 rec=1 t=6.01s | ok w=23 rec=1 t=3.20s | ok w=17 rec=0 t=1.11s | ok w=20 rec=0 t=3.09s | ok w=26 rec=1 t=2.39s | ok w=20 rec=1 t=3.40s | ok w=21 rec=3 t=3.97s |
| rockclimb | ok w=407 rec=13 t=32.80s | ok w=546 rec=10 t=39.82s | ok w=929 rec=9 t=35.76s | ok w=747 rec=0 t=150.14s | ok w=537 rec=35 t=110.39s | ok w=526 rec=9 t=47.58s | ok w=514 rec=0 t=83.07s | ok w=797 rec=81 t=125.61s | ok w=485 rec=0 t=120.84s | ok w=511 rec=81 t=106.49s |
| schematic | ok w=49 rec=1 t=2.70s | ok w=59 rec=1 t=4.30s | ok w=87 rec=0 t=1.30s | ok w=66 rec=0 t=13.93s | ok w=61 rec=3 t=11.18s | ok w=54 rec=1 t=4.69s | ok w=57 rec=0 t=9.02s | ok w=76 rec=6 t=10.15s | ok w=51 rec=0 t=10.92s | ok w=56 rec=6 t=8.08s |
| schematicO3 | ok w=20 rec=0 t=0.86s | ok w=24 rec=0 t=0.77s | ok w=40 rec=0 t=0.59s | ok w=33 rec=0 t=6.06s | ok w=26 rec=1 t=4.96s | ok w=23 rec=0 t=1.17s | ok w=24 rec=0 t=5.08s | ok w=32 rec=3 t=5.40s | ok w=26 rec=0 t=5.83s | ok w=25 rec=3 t=4.01s |

### dijkstra (baseline result 3788)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=9 rec=0 t=0.11s | ok w=10 rec=0 t=0.54s | ok w=10 rec=0 t=0.14s | ok w=9 rec=0 t=1.46s | ok w=10 rec=0 t=1.85s | ok w=9 rec=0 t=0.70s | ok w=10 rec=0 t=2.02s | ok w=12 rec=0 t=0.87s | ok w=9 rec=0 t=0.91s | ok w=9 rec=0 t=0.11s |
| rockclimb | ok w=512 rec=16 t=40.52s | ok w=676 rec=13 t=51.34s | ok w=1210 rec=11 t=44.71s | ok w=931 rec=0 t=189.48s | ok w=699 rec=44 t=138.28s | ok w=646 rec=11 t=58.44s | ok w=640 rec=0 t=103.98s | ok w=992 rec=102 t=157.86s | ok w=620 rec=0 t=150.83s | ok w=632 rec=99 t=130.22s |
| schematic | ok w=52 rec=1 t=2.74s | ok w=60 rec=1 t=4.34s | ok w=97 rec=0 t=1.38s | ok w=75 rec=0 t=16.48s | ok w=66 rec=4 t=12.41s | ok w=62 rec=1 t=5.18s | ok w=61 rec=0 t=9.17s | ok w=85 rec=7 t=11.60s | ok w=61 rec=0 t=13.35s | ok w=65 rec=9 t=11.87s |
| schematicO3 | ok w=10 rec=0 t=0.13s | ok w=11 rec=0 t=0.57s | ok w=14 rec=0 t=0.18s | ok w=12 rec=0 t=2.62s | ok w=12 rec=0 t=1.87s | ok w=12 rec=0 t=0.88s | ok w=13 rec=0 t=2.05s | ok w=14 rec=0 t=0.89s | ok w=10 rec=0 t=0.93s | ok w=9 rec=0 t=0.12s |

### qsort (baseline result 9987)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=11 rec=0 t=0.14s | ok w=11 rec=0 t=0.57s | ok w=12 rec=0 t=0.16s | ok w=14 rec=0 t=3.40s | ok w=12 rec=0 t=1.88s | ok w=12 rec=0 t=0.72s | ok w=12 rec=0 t=2.05s | ok w=16 rec=0 t=0.89s | ok w=11 rec=0 t=0.94s | ok w=10 rec=0 t=0.13s |
| rockclimb | ok w=104 rec=3 t=7.71s | ok w=136 rec=2 t=8.73s | ok w=236 rec=2 t=8.30s | ok w=205 rec=0 t=40.14s | ok w=138 rec=9 t=27.96s | ok w=135 rec=2 t=11.46s | ok w=133 rec=0 t=20.89s | ok w=199 rec=19 t=30.07s | ok w=132 rec=0 t=30.84s | ok w=127 rec=18 t=23.84s |
| schematic | ok w=17 rec=0 t=0.22s | ok w=20 rec=0 t=0.73s | ok w=28 rec=0 t=0.34s | ok w=32 rec=0 t=6.03s | ok w=23 rec=1 t=3.18s | ok w=22 rec=0 t=1.13s | ok w=24 rec=0 t=3.49s | ok w=27 rec=1 t=2.41s | ok w=20 rec=0 t=3.41s | ok w=22 rec=3 t=3.95s |
| schematicO3 | ok w=15 rec=0 t=0.19s | ok w=19 rec=0 t=0.70s | ok w=25 rec=0 t=0.30s | ok w=23 rec=0 t=4.08s | ok w=22 rec=1 t=3.17s | ok w=21 rec=0 t=1.12s | ok w=20 rec=0 t=3.06s | ok w=25 rec=1 t=2.38s | ok w=18 rec=0 t=3.38s | ok w=15 rec=0 t=0.19s |

### activity_recognition (baseline result 64)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=11 rec=0 t=0.14s | ok w=13 rec=0 t=0.62s | ok w=13 rec=0 t=0.18s | ok w=12 rec=0 t=3.27s | ok w=14 rec=0 t=1.90s | ok w=13 rec=0 t=1.03s | ok w=13 rec=0 t=2.06s | ok w=17 rec=0 t=0.92s | ok w=12 rec=0 t=2.63s | ok w=11 rec=0 t=0.14s |
| rockclimb | ok w=49 rec=1 t=2.74s | ok w=66 rec=1 t=4.48s | ok w=110 rec=1 t=4.06s | ok w=90 rec=0 t=19.13s | ok w=64 rec=4 t=12.46s | ok w=64 rec=1 t=5.83s | ok w=62 rec=0 t=10.03s | ok w=92 rec=9 t=14.62s | ok w=60 rec=0 t=13.38s | ok w=61 rec=9 t=11.86s |
| schematic | ok w=44 rec=1 t=2.65s | ok w=55 rec=0 t=2.09s | ok w=87 rec=0 t=1.28s | ok w=64 rec=0 t=13.90s | ok w=54 rec=3 t=11.14s | ok w=51 rec=1 t=3.78s | ok w=51 rec=0 t=8.98s | ok w=73 rec=6 t=10.12s | ok w=52 rec=0 t=10.88s | ok w=49 rec=6 t=8.03s |
| schematicO3 | ok w=14 rec=0 t=0.18s | ok w=16 rec=0 t=0.65s | ok w=21 rec=0 t=0.26s | ok w=18 rec=0 t=3.45s | ok w=18 rec=1 t=3.13s | ok w=18 rec=0 t=1.10s | ok w=17 rec=0 t=3.04s | ok w=21 rec=1 t=2.34s | ok w=16 rec=1 t=3.36s | ok w=13 rec=0 t=0.17s |

### bitcount (baseline result 14121)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=8 rec=0 t=0.11s | ok w=8 rec=0 t=0.55s | ok w=7 rec=0 t=0.12s | ok w=8 rec=0 t=0.85s | ok w=9 rec=0 t=1.84s | ok w=10 rec=0 t=0.70s | ok w=9 rec=0 t=2.01s | ok w=14 rec=0 t=0.88s | ok w=9 rec=0 t=0.92s | ok w=8 rec=0 t=0.11s |
| rockclimb | ok w=280 rec=9 t=22.70s | ok w=381 rec=7 t=27.97s | ok w=663 rec=6 t=24.45s | ok w=532 rec=0 t=116.11s | ok w=388 rec=24 t=76.25s | ok w=352 rec=6 t=32.19s | ok w=354 rec=0 t=57.55s | ok w=541 rec=54 t=84.08s | ok w=327 rec=0 t=80.89s | ok w=346 rec=54 t=71.04s |
| schematic | ok w=32 rec=0 t=1.02s | ok w=40 rec=0 t=1.69s | ok w=63 rec=0 t=1.01s | ok w=53 rec=0 t=11.25s | ok w=43 rec=2 t=8.06s | ok w=39 rec=0 t=1.60s | ok w=41 rec=0 t=6.52s | ok w=55 rec=4 t=6.99s | ok w=40 rec=0 t=8.37s | ok w=41 rec=6 t=7.89s |
| schematicO3 | ok w=9 rec=0 t=0.12s | ok w=11 rec=0 t=0.57s | ok w=9 rec=0 t=0.14s | ok w=10 rec=0 t=2.62s | ok w=11 rec=0 t=1.88s | ok w=12 rec=0 t=0.72s | ok w=10 rec=0 t=2.02s | ok w=14 rec=0 t=0.89s | ok w=9 rec=0 t=0.92s | ok w=9 rec=0 t=0.12s |

### chacha20 (baseline result 79)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=11 rec=0 t=0.16s | ok w=12 rec=0 t=0.62s | ok w=11 rec=0 t=0.21s | ok w=12 rec=1 t=3.39s | ok w=11 rec=0 t=1.90s | ok w=11 rec=0 t=1.04s | ok w=11 rec=0 t=2.08s | ok w=13 rec=0 t=0.91s | ok w=12 rec=1 t=2.50s | ok w=11 rec=0 t=0.16s |
| rockclimb | ok w=46 rec=1 t=2.69s | ok w=61 rec=1 t=4.31s | ok w=96 rec=0 t=1.39s | ok w=72 rec=0 t=13.94s | ok w=57 rec=3 t=11.18s | ok w=55 rec=1 t=4.67s | ok w=55 rec=0 t=9.03s | ok w=75 rec=6 t=10.17s | ok w=56 rec=0 t=10.94s | ok w=53 rec=6 t=8.06s |
| schematic | ok w=34 rec=1 t=2.52s | ok w=34 rec=0 t=1.67s | ok w=37 rec=0 t=0.77s | ok w=35 rec=1 t=8.65s | ok w=36 rec=2 t=6.27s | ok w=33 rec=0 t=1.61s | ok w=34 rec=0 t=5.67s | ok w=37 rec=3 t=5.52s | ok w=37 rec=2 t=8.33s | ok w=39 rec=6 t=7.88s |
| schematicO3 | ok w=22 rec=0 t=0.89s | ok w=24 rec=0 t=0.78s | ok w=28 rec=0 t=0.37s | ok w=25 rec=0 t=6.04s | ok w=27 rec=1 t=4.97s | ok w=23 rec=0 t=1.18s | ok w=23 rec=0 t=5.08s | ok w=31 rec=3 t=5.38s | ok w=24 rec=1 t=5.12s | ok w=25 rec=3 t=4.01s |

### sensor_fusion (baseline result 613)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=8 rec=0 t=0.10s | ok w=12 rec=0 t=0.55s | ok w=13 rec=0 t=0.17s | ok w=11 rec=0 t=2.62s | ok w=11 rec=0 t=1.85s | ok w=11 rec=0 t=0.71s | ok w=11 rec=0 t=2.02s | ok w=14 rec=0 t=0.88s | ok w=9 rec=0 t=0.92s | ok w=8 rec=0 t=0.10s |
| rockclimb | ok w=10 rec=0 t=0.13s | ok w=12 rec=0 t=0.57s | ok w=14 rec=0 t=0.18s | ok w=14 rec=0 t=3.40s | ok w=12 rec=0 t=1.87s | ok w=14 rec=0 t=1.04s | ok w=13 rec=0 t=2.05s | ok w=16 rec=0 t=0.90s | ok w=10 rec=0 t=0.93s | ok w=10 rec=0 t=0.13s |
| schematic | ok w=14 rec=0 t=0.18s | ok w=16 rec=0 t=0.64s | ok w=20 rec=0 t=0.25s | ok w=18 rec=0 t=3.44s | ok w=17 rec=1 t=3.11s | ok w=17 rec=0 t=1.08s | ok w=17 rec=0 t=3.03s | ok w=19 rec=0 t=0.93s | ok w=15 rec=0 t=3.36s | ok w=14 rec=0 t=0.18s |
| schematicO3 | ok w=12 rec=0 t=0.15s | ok w=16 rec=0 t=0.64s | ok w=20 rec=0 t=0.24s | ok w=22 rec=0 t=3.44s | ok w=16 rec=1 t=3.10s | ok w=18 rec=0 t=1.06s | ok w=15 rec=0 t=2.07s | ok w=19 rec=0 t=0.93s | ok w=15 rec=0 t=3.35s | ok w=12 rec=0 t=0.15s |

### poly1305 (baseline result 68)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=132 rec=4 t=10.19s | ok w=131 rec=2 t=9.70s | ok w=129 rec=1 t=5.16s | ok w=133 rec=5 t=36.68s | ok w=138 rec=9 t=29.77s | ok w=131 rec=3 t=11.92s | ok w=128 rec=0 t=22.96s | ok w=148 rec=18 t=28.60s | ok w=139 rec=10 t=33.34s | ok w=152 rec=24 t=31.67s |
| rockclimb | ok w=322 rec=9 t=23.53s | ok w=407 rec=7 t=28.00s | ok w=651 rec=6 t=24.23s | ok w=518 rec=0 t=116.07s | ok w=407 rec=25 t=79.38s | ok w=383 rec=6 t=32.61s | ok w=385 rec=0 t=60.11s | ok w=564 rec=55 t=85.52s | ok w=362 rec=0 t=83.40s | ok w=386 rec=57 t=75.01s |
| schematic | ok w=269 rec=8 t=20.23s | ok w=281 rec=5 t=20.35s | ok w=335 rec=3 t=13.01s | ok w=298 rec=1 t=68.90s | ok w=291 rec=19 t=60.78s | ok w=287 rec=7 t=26.86s | ok w=276 rec=0 t=43.83s | ok w=316 rec=37 t=57.77s | ok w=301 rec=20 t=68.36s | ok w=307 rec=48 t=63.05s |
| schematicO3 | ok w=185 rec=5 t=13.50s | ok w=191 rec=3 t=13.84s | ok w=241 rec=2 t=9.22s | ok w=189 rec=0 t=50.59s | ok w=214 rec=14 t=43.45s | ok w=207 rec=5 t=20.10s | ok w=191 rec=0 t=32.95s | ok w=244 rec=28 t=43.96s | ok w=203 rec=7 t=48.36s | ok w=216 rec=33 t=43.48s |

### cuckoo_filter (baseline result 4048)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=13 rec=0 t=0.18s | ok w=14 rec=0 t=0.65s | ok w=16 rec=0 t=0.23s | ok w=16 rec=0 t=3.43s | ok w=13 rec=0 t=1.91s | ok w=13 rec=0 t=1.05s | ok w=15 rec=0 t=2.08s | ok w=18 rec=0 t=0.93s | ok w=15 rec=1 t=3.35s | ok w=13 rec=0 t=0.18s |
| rockclimb | ok w=130 rec=4 t=10.12s | ok w=170 rec=3 t=12.18s | ok w=297 rec=2 t=9.21s | ok w=234 rec=0 t=47.94s | ok w=170 rec=11 t=34.15s | ok w=164 rec=3 t=14.09s | ok w=171 rec=0 t=26.38s | ok w=248 rec=24 t=37.80s | ok w=155 rec=0 t=35.89s | ok w=159 rec=24 t=31.59s |
| schematic | ok w=117 rec=4 t=7.75s | ok w=125 rec=2 t=8.56s | ok w=175 rec=1 t=5.27s | ok w=139 rec=0 t=32.28s | ok w=132 rec=8 t=24.89s | ok w=128 rec=2 t=11.41s | ok w=126 rec=0 t=20.43s | ok w=158 rec=16 t=25.51s | ok w=126 rec=3 t=28.38s | ok w=134 rec=18 t=23.83s |
| schematicO3 | ok w=18 rec=1 t=0.84s | ok w=22 rec=0 t=0.74s | ok w=26 rec=0 t=0.36s | ok w=24 rec=1 t=6.01s | ok w=23 rec=1 t=3.20s | ok w=19 rec=0 t=1.14s | ok w=22 rec=0 t=3.49s | ok w=25 rec=1 t=2.40s | ok w=20 rec=0 t=3.43s | ok w=22 rec=3 t=3.97s |

### sha256 (baseline result 131)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=18 rec=1 t=0.87s | ok w=18 rec=0 t=0.75s | ok w=18 rec=0 t=0.34s | ok w=19 rec=2 t=5.24s | ok w=18 rec=1 t=3.19s | ok w=17 rec=0 t=1.14s | ok w=17 rec=0 t=3.09s | ok w=20 rec=1 t=2.41s | ok w=19 rec=1 t=3.44s | ok w=20 rec=3 t=3.99s |
| rockclimb | ok w=74 rec=2 t=5.14s | ok w=99 rec=1 t=5.60s | ok w=172 rec=1 t=4.96s | ok w=130 rec=0 t=24.43s | ok w=98 rec=6 t=18.64s | ok w=91 rec=1 t=6.61s | ok w=93 rec=0 t=14.87s | ok w=138 rec=12 t=19.38s | ok w=91 rec=0 t=20.83s | ok w=87 rec=12 t=15.90s |
| schematic | ok w=38 rec=1 t=2.59s | ok w=45 rec=0 t=1.92s | ok w=61 rec=0 t=1.08s | ok w=49 rec=1 t=11.29s | ok w=46 rec=3 t=9.30s | ok w=39 rec=0 t=1.68s | ok w=45 rec=0 t=8.57s | ok w=56 rec=6 t=9.28s | ok w=41 rec=1 t=8.42s | ok w=44 rec=6 t=7.97s |
| schematicO3 | ok w=35 rec=1 t=2.53s | ok w=33 rec=0 t=1.65s | ok w=33 rec=0 t=0.66s | ok w=35 rec=2 t=8.64s | ok w=35 rec=2 t=6.26s | ok w=33 rec=0 t=1.63s | ok w=33 rec=0 t=5.54s | ok w=37 rec=3 t=5.52s | ok w=35 rec=1 t=8.23s | ok w=39 rec=6 t=7.90s |

### stringsearch (baseline result 20)

| algo | tr1 | tr2 | tr3 | tr4 | tr5 | tr6 | tr7 | tr8 | tr9 | tr10 |
|---|---|---|---|---|---|---|---|---|---|---|
| milp | ok w=14 rec=0 t=0.18s | ok w=19 rec=0 t=0.69s | ok w=24 rec=0 t=0.30s | ok w=21 rec=0 t=3.47s | ok w=20 rec=1 t=3.14s | ok w=19 rec=0 t=1.10s | ok w=20 rec=0 t=3.06s | ok w=25 rec=1 t=2.36s | ok w=18 rec=0 t=3.39s | ok w=14 rec=0 t=0.18s |
| rockclimb | ok w=594 rec=19 t=47.92s | ok w=804 rec=14 t=56.57s | ok w=1393 rec=13 t=52.44s | ok w=1170 rec=0 t=241.87s | ok w=789 rec=52 t=163.03s | ok w=758 rec=13 t=68.56s | ok w=791 rec=0 t=127.35s | ok w=1153 rec=118 t=182.56s | ok w=716 rec=0 t=173.35s | ok w=737 rec=117 t=153.78s |
| schematic | ok w=55 rec=1 t=3.40s | ok w=68 rec=1 t=4.48s | ok w=94 rec=0 t=1.40s | ok w=71 rec=0 t=16.37s | ok w=67 rec=4 t=12.48s | ok w=66 rec=1 t=5.83s | ok w=61 rec=0 t=10.04s | ok w=81 rec=7 t=11.63s | ok w=63 rec=0 t=13.40s | ok w=65 rec=9 t=11.91s |
| schematicO3 | ok w=16 rec=0 t=0.21s | ok w=21 rec=0 t=0.72s | ok w=28 rec=0 t=0.34s | ok w=30 rec=0 t=6.02s | ok w=22 rec=1 t=3.18s | ok w=22 rec=0 t=1.12s | ok w=23 rec=0 t=3.08s | ok w=27 rec=1 t=2.40s | ok w=21 rec=0 t=3.41s | ok w=22 rec=3 t=3.95s |


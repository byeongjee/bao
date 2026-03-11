#!/usr/bin/env bash
#
# Benchmark SCHEMATIC checkpoint insertion across all intermittent benchmarks
# and capacitor sizes. Compiles via compile_schematic.sh, flashes to MSP430
# via mspdebug, and reads runtime counters over UART.
#
# Outputs a CSV summary.
#
# Pipeline per benchmark:
#   1. Collect trace once via compile_schematic.sh --trace-only
#   2. Per capacitor: compile with --link --debug-counters, flash, read serial
#
# Usage:
#   ./scripts/run_schematic.sh [--debug-counters] [--halt-mode] [--cap <size>] [-o output.csv] [-v|--verbose] [bench1 bench2 ...]
#
# Options:
#   --debug-counters  Link debug counter runtime (UART output + NVM counters).
#   --halt-mode       Enable LPM4 halt at region boundaries.
#   --cap <size>      Run only the given capacitor size (1uF, 10uF, 100uF).
#                     Can be repeated: --cap 1uF --cap 10uF
#
# If benchmark names are given, only those are run (matched by filename without .c).
# Example: ./scripts/run_schematic.sh --debug-counters sha256 aes
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

OUTPUT_CSV="$PROJECT_DIR/benchmarks/schematic_benchmark_summary.csv"
VERBOSE=0
DEBUG_COUNTERS=0
HALT_MODE=0
FILTER_BENCHMARKS=()
FILTER_CAPS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        -o) OUTPUT_CSV="$2"; shift 2 ;;
        -v|--verbose) VERBOSE=1; shift ;;
        --debug-counters) DEBUG_COUNTERS=1; shift ;;
        --halt-mode) HALT_MODE=1; shift ;;
        --cap) FILTER_CAPS+=("$2"); shift 2 ;;
        -h|--help) echo "Usage: $0 [--debug-counters] [--halt-mode] [--cap <size>] [-o output.csv] [-v|--verbose] [bench1 bench2 ...]"; exit 0 ;;
        -*) echo "Unknown option: $1" >&2; exit 1 ;;
        *) FILTER_BENCHMARKS+=("$1"); shift ;;
    esac
done

ENERGY_CONFIG="$PROJECT_DIR/benchmarks/sample_energy_config_ir.json"

ALL_CAPACITOR_CONFIGS=(
    "1uF:$PROJECT_DIR/benchmarks/sample_schematic_config_1uF.json"
    "10uF:$PROJECT_DIR/benchmarks/sample_schematic_config_10uF.json"
    "100uF:$PROJECT_DIR/benchmarks/sample_schematic_config_100uF.json"
)

# Filter capacitor configs if --cap specified
CAPACITOR_CONFIGS=()
if [[ ${#FILTER_CAPS[@]} -gt 0 ]]; then
    for cap_entry in "${ALL_CAPACITOR_CONFIGS[@]}"; do
        cap_label="${cap_entry%%:*}"
        for fc in "${FILTER_CAPS[@]}"; do
            if [[ "$cap_label" == "$fc" ]]; then
                CAPACITOR_CONFIGS+=("$cap_entry")
                break
            fi
        done
    done
    if [[ ${#CAPACITOR_CONFIGS[@]} -eq 0 ]]; then
        echo "Error: No matching capacitor sizes. Available: 1uF, 10uF, 100uF" >&2
        exit 1
    fi
else
    CAPACITOR_CONFIGS=("${ALL_CAPACITOR_CONFIGS[@]}")
fi

# Find benchmarks
BENCHMARKS=()
if [[ ${#FILTER_BENCHMARKS[@]} -gt 0 ]]; then
    for name in "${FILTER_BENCHMARKS[@]}"; do
        f="$PROJECT_DIR/benchmarks/intermittent/${name}.c"
        if [[ -f "$f" ]]; then
            BENCHMARKS+=("$f")
        else
            echo "Warning: Benchmark not found: $f" >&2
        fi
    done
else
    for f in "$PROJECT_DIR"/benchmarks/intermittent/*.c; do
        [[ -f "$f" ]] && BENCHMARKS+=("$f")
    done
fi

if [[ ${#BENCHMARKS[@]} -eq 0 ]]; then
    echo "Error: No benchmarks to run" >&2
    exit 1
fi

# CSV header
HEADER="benchmark,capacitor,status,basic_blocks,edges,regions,compilation_time_ms,peak_rss_kb,profiling_time_ms,execution_time_ms,boundary_checks,runtime_region_boundary_calls,runtime_debug_save_reg_calls,runtime_debug_restore_reg_calls,runtime_debug_store_mem_calls,runtime_debug_restore_mem_calls,candidate_globals,enabled_checkpoints,loop_decisions,paths_analyzed,runtime_calls_inserted"
echo "$HEADER" > "$OUTPUT_CSV"

FAIL_COLS=",,,,,,,,,,,,,,,,,,"  # 18 empty fields for error rows

# Extract first numeric/token value after "label:" from output.
extract_stat() {
    local output="$1"
    shift

    local label line value
    for label in "$@"; do
        line=$(echo "$output" | grep -F "$label:" | head -1 || true)
        if [[ -n "$line" ]]; then
            value="${line#*:}"
            value=$(echo "$value" | awk '{print $1}')
            if [[ -n "$value" ]]; then
                echo "$value"
                return 0
            fi
        fi
    done
    return 1
}

# Extract runtime counter value from debug counter output.
extract_counter() {
    local output="$1"
    local label="$2"
    local line value
    line=$(echo "$output" | grep -F "$label:" | head -1 || true)
    if [[ -n "$line" ]]; then
        value=$(echo "$line" | awk '{print $NF}')
        echo "$value"
    else
        echo "0"
    fi
}

TMPDIR="${TMPDIR:-/tmp}/schematic_bench_$$"
mkdir -p "$TMPDIR"

total=$((${#BENCHMARKS[@]} * ${#CAPACITOR_CONFIGS[@]}))
count=0

for bench_path in "${BENCHMARKS[@]}"; do
    bench_name=$(basename "$bench_path" .c)

    # === Per-benchmark: collect trace once ===
    echo ""
    echo "--- Collecting trace for $bench_name ---"

    trace_json="$TMPDIR/${bench_name}_trace.json"
    VERBOSE_FLAG=""
    [[ "$VERBOSE" -eq 1 ]] && VERBOSE_FLAG="--verbose"

    trace_output=$("$SCRIPT_DIR/compile_schematic.sh" \
        -e "$ENERGY_CONFIG" \
        -s "$PROJECT_DIR/benchmarks/sample_schematic_config_10uF.json" \
        -o "$TMPDIR/${bench_name}" \
        -Oc 0 \
        --local $VERBOSE_FLAG \
        -I "$PROJECT_DIR/passes/runtime" \
        --trace-only \
        "$bench_path" 2>&1) || true

    if [[ "$VERBOSE" -eq 1 ]]; then
        echo "$trace_output"
    fi

    # Extract profiling time from trace collection output
    PROFILING_TIME_MS=$(echo "$trace_output" | sed -n 's/.*Profiling time (ms): \([0-9]*\).*/\1/p')
    PROFILING_TIME_MS=${PROFILING_TIME_MS:-0}

    if [[ ! -f "$TMPDIR/${bench_name}_trace.json" ]]; then
        echo "  FAILED: trace collection for $bench_name"
        echo "$trace_output" | tail -5
        for cap_entry in "${CAPACITOR_CONFIGS[@]}"; do
            cap_label="${cap_entry%%:*}"
            count=$((count + 1))
            echo "${bench_name}-${cap_label},${cap_label},TRACE_FAILED${FAIL_COLS}" >> "$OUTPUT_CSV"
        done
        continue
    fi
    mv "$TMPDIR/${bench_name}_trace.json" "$trace_json"
    echo "  Trace collected for $bench_name (profiling: ${PROFILING_TIME_MS}ms)"

    # === Per-capacitor: compile, flash, read serial ===
    for cap_entry in "${CAPACITOR_CONFIGS[@]}"; do
        cap_label="${cap_entry%%:*}"
        cap_config="${cap_entry#*:}"
        count=$((count + 1))

        row_name="${bench_name}-${cap_label}"
        echo "[$count/$total] Running $row_name ..."

        if [[ ! -f "$cap_config" ]]; then
            echo "  SKIPPED: config not found: $cap_config"
            echo "$row_name,$cap_label,CONFIG_NOT_FOUND${FAIL_COLS}" >> "$OUTPUT_CSV"
            continue
        fi

        # Build compile command
        run_cmd=(
            "$SCRIPT_DIR/compile_schematic.sh"
            -e "$ENERGY_CONFIG"
            -s "$cap_config"
            -o "$TMPDIR/$bench_name"
            -Oc 0
            --trace "$trace_json"
            --add-debug-markers
            --link
            -I "$PROJECT_DIR/passes/runtime"
        )
        [[ "$VERBOSE" -eq 1 ]] && run_cmd+=(--verbose)
        [[ "$DEBUG_COUNTERS" -eq 1 ]] && run_cmd+=(--debug-counters)
        [[ "$HALT_MODE" -eq 1 ]] && run_cmd+=(--halt-mode)
        run_cmd+=("$bench_path")

        printf -v run_cmd_display '%q ' "${run_cmd[@]}"
        run_cmd_display="${run_cmd_display% }"
        echo "  Command: $run_cmd_display"

        compile_output=$("${run_cmd[@]}" 2>&1) || true

        if [[ "$VERBOSE" -eq 1 ]]; then
            echo "$compile_output"
        fi

        # Flash via mspdebug
        flash_output=""
        if [[ -f "$TMPDIR/${bench_name}.elf" ]]; then
            flash_output=$(mspdebug tilib "prog $TMPDIR/${bench_name}.elf" 2>&1) || true
        fi

        # Read serial output (debug-counters mode — real runtime halts in LPM4, no UART)
        serial_output=""
        if [[ "$DEBUG_COUNTERS" -eq 1 && -f "$TMPDIR/${bench_name}.elf" ]]; then
            serial_output=$(uv run python3 "$SCRIPT_DIR/lib/read_serial.py" \
                --timeout 30 \
                --reset-cmd "mspdebug tilib \"reset\" \"run\"" \
                2>"$TMPDIR/serial.err") || true
        fi

        # Merge compile + serial output for stat extraction
        full_output="${compile_output}
${serial_output}"

        if [[ "$VERBOSE" -eq 1 ]]; then
            echo "$full_output"
        fi

        # Check for infeasibility
        if echo "$full_output" | grep -q "SCHEMATIC infeasible"; then
            echo "  INFEASIBLE (energy capacity too small)"
            echo "$row_name,$cap_label,infeasible${FAIL_COLS}" >> "$OUTPUT_CSV"
            continue
        fi

        # Check for failure
        if ! echo "$full_output" | grep -q "Checkpoint Insertion Statistics"; then
            echo "  FAILED (SCHEMATIC pass error)"
            [[ "$VERBOSE" -eq 0 ]] && echo "$full_output" | tail -5
            echo "$row_name,$cap_label,failed${FAIL_COLS}" >> "$OUTPUT_CSV"
            continue
        fi

        basic_blocks=$(extract_stat "$full_output" "Basic blocks")
        edges=$(extract_stat "$full_output" "Edges")
        candidate_globals=$(extract_stat "$full_output" "Candidate globals (V_elig)")
        enabled_ckpts=$(extract_stat "$full_output" "Enabled checkpoints")
        loop_decisions=$(extract_stat "$full_output" "Loop decisions")
        regions=$(extract_stat "$full_output" "Regions")
        paths_analyzed=$(extract_stat "$full_output" "Paths analyzed")
        runtime_calls=$(extract_stat "$full_output" "Runtime calls inserted")
        compilation_time=$(extract_stat "$full_output" "Compilation time (ms)")
        compilation_time=${compilation_time:-0}
        peak_rss=$(extract_stat "$full_output" "Peak RSS (KB)")
        peak_rss=${peak_rss:-0}
        execution_time=$(extract_stat "$full_output" "Execution time (ms)")
        execution_time=${execution_time:-0}
        boundary_checks=$(extract_stat "$full_output" "Boundary checks")
        boundary_checks=${boundary_checks:-0}

        # Extract debug counter stats (same labels as schematic_debug_counters.c)
        rt_boundary=$(extract_counter "$full_output" "__region_boundary")
        rt_save_reg=$(extract_counter "$full_output" "reg_saves")
        rt_restore_reg=$(extract_counter "$full_output" "reg_restores")
        rt_store_mem=$(extract_counter "$full_output" "mem_stores")
        rt_restore_mem=$(extract_counter "$full_output" "mem_restores")

        echo "$row_name,$cap_label,ok,$basic_blocks,$edges,$regions,$compilation_time,$peak_rss,$PROFILING_TIME_MS,$execution_time,$boundary_checks,$rt_boundary,$rt_save_reg,$rt_restore_reg,$rt_store_mem,$rt_restore_mem,$candidate_globals,$enabled_ckpts,$loop_decisions,$paths_analyzed,$runtime_calls" >> "$OUTPUT_CSV"
        echo "  OK ($regions regions, $enabled_ckpts checkpoints, $runtime_calls runtime calls, $rt_boundary boundaries)"
    done
done

# Cleanup
rm -rf "$TMPDIR"

echo ""
echo "=========================================="
echo "Results written to: $OUTPUT_CSV"
echo "=========================================="
echo ""
column -t -s',' "$OUTPUT_CSV"

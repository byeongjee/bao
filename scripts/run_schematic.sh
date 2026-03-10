#!/usr/bin/env bash
#
# Benchmark SCHEMATIC checkpoint insertion across all intermittent benchmarks
# and capacitor sizes. Outputs a CSV summary.
#
# Pipeline per benchmark:
#   1. Collect trace once via compile_schematic.sh --trace-only
#   2. Per capacitor: compile_and_run.sh --mode schematic -t <trace.json>
#
# Usage:
#   ./scripts/run_schematic.sh [-o output.csv] [-v|--verbose] [bench1 bench2 ...]
#
# If benchmark names are given, only those are run (matched by filename without .c).
# Example: ./scripts/run_schematic.sh sha256 aes
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

OUTPUT_CSV="$PROJECT_DIR/benchmarks/schematic_benchmark_summary.csv"
VERBOSE=0
FILTER_BENCHMARKS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        -o) OUTPUT_CSV="$2"; shift 2 ;;
        -v|--verbose) VERBOSE=1; shift ;;
        -h|--help) echo "Usage: $0 [-o output.csv] [-v|--verbose] [bench1 bench2 ...]"; exit 0 ;;
        -*) echo "Unknown option: $1" >&2; exit 1 ;;
        *) FILTER_BENCHMARKS+=("$1"); shift ;;
    esac
done

ENERGY_CONFIG="$PROJECT_DIR/benchmarks/sample_energy_config_ir.json"

CAPACITOR_CONFIGS=(
    "1uF:$PROJECT_DIR/benchmarks/sample_schematic_config_1uF.json"
    "10uF:$PROJECT_DIR/benchmarks/sample_schematic_config_10uF.json"
    "100uF:$PROJECT_DIR/benchmarks/sample_schematic_config_100uF.json"
)

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
HEADER="benchmark,capacitor,status,basic_blocks,edges,regions,compilation_time_ms,peak_rss_kb,profiling_time_ms,execution_time_ms,runtime_region_prologue_calls,runtime_region_epilogue_calls,runtime_checkpoint_store_reg_calls,runtime_checkpoint_store_mem_calls,runtime_restore_reg_calls,runtime_restore_mem_calls,candidate_globals,enabled_checkpoints,loop_decisions,paths_analyzed,runtime_calls_inserted"
echo "$HEADER" > "$OUTPUT_CSV"

FAIL_COLS=",,,,,,,,,,,,,,,,,,,,"  # 19 empty fields for error rows

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

# Extract runtime counter value from mock counter output.
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

    # === Per-capacitor: compile + run via orchestrator ===
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

        run_cmd=(
            "$SCRIPT_DIR/compile_and_run.sh"
            --mode schematic
            --runtime mock-counter
            -e "$ENERGY_CONFIG"
            -s "$cap_config"
            -t "$trace_json"
            -Oc 0
            --local
            -I "$PROJECT_DIR/passes/runtime"
            --verbose
            "$bench_path"
        )
        printf -v run_cmd_display '%q ' "${run_cmd[@]}"
        run_cmd_display="${run_cmd_display% }"
        echo "  Command: $run_cmd_display"

        full_output=$("${run_cmd[@]}" 2>&1) || true

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

        # Extract mock counter stats
        rt_prologue=$(extract_counter "$full_output" "__region_prologue")
        rt_epilogue=$(extract_counter "$full_output" "__region_epilogue")
        rt_store_reg=$(extract_counter "$full_output" "__checkpoint_store_reg")
        rt_store_mem=$(extract_counter "$full_output" "__checkpoint_store_mem")
        rt_restore_reg=$(extract_counter "$full_output" "__restore_reg")
        rt_restore_mem=$(extract_counter "$full_output" "__restore_mem")

        echo "$row_name,$cap_label,ok,$basic_blocks,$edges,$regions,$compilation_time,$peak_rss,$PROFILING_TIME_MS,$execution_time,$rt_prologue,$rt_epilogue,$rt_store_reg,$rt_store_mem,$rt_restore_reg,$rt_restore_mem,$candidate_globals,$enabled_ckpts,$loop_decisions,$paths_analyzed,$runtime_calls" >> "$OUTPUT_CSV"
        echo "  OK ($regions regions, $enabled_ckpts checkpoints, $runtime_calls runtime calls, $rt_prologue prologues)"
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

#!/usr/bin/env bash
#
# Benchmark SCHEMATIC checkpoint insertion across all intermittent benchmarks
# and capacitor sizes. Outputs a CSV summary.
#
# Usage:
#   ./scripts/benchmark_schematic.sh [-o output.csv] [-v|--verbose] [bench1 bench2 ...]
#
# If benchmark names are given, only those are run (matched by filename without .c).
# Example: ./scripts/benchmark_schematic.sh sha256 aes
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
    "100nF:$PROJECT_DIR/benchmarks/sample_schematic_config_100nF.json"
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
HEADER="benchmark,capacitor,basic_blocks,edges,candidate_globals,regions,checkpoints_placed,paths_analyzed,vm_variables,nvm_variables,total_execution_time_ms,runtime_region_prologue_calls,runtime_region_epilogue_calls,runtime_checkpoint_store_reg_calls,runtime_checkpoint_store_mem_calls,runtime_restore_reg_calls,runtime_restore_mem_calls"
echo "$HEADER" > "$OUTPUT_CSV"

# Extract first numeric/token value after "label:" from output.
# Accepts multiple labels and returns the first match.
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

total=$((${#BENCHMARKS[@]} * ${#CAPACITOR_CONFIGS[@]}))
count=0

for bench_path in "${BENCHMARKS[@]}"; do
    bench_name=$(basename "$bench_path" .c)

    for cap_entry in "${CAPACITOR_CONFIGS[@]}"; do
        cap_label="${cap_entry%%:*}"
        cap_config="${cap_entry#*:}"
        count=$((count + 1))

        row_name="${bench_name}-${cap_label}"
        echo "[$count/$total] Running $row_name ..."

        run_cmd=(
            "$SCRIPT_DIR/compile_and_flash.sh"
            --mode schematic
            --runtime mock-counter
            -e "$ENERGY_CONFIG"
            -s "$cap_config"
            --local
            -I "$PROJECT_DIR/passes/runtime"
            --verbose
            "$bench_path"
        )
        printf -v run_cmd_display '%q ' "${run_cmd[@]}"
        run_cmd_display="${run_cmd_display% }"
        echo "  Command: $run_cmd_display"

        # Run compile_and_flash, capture all output regardless of exit code
        full_output=$("${run_cmd[@]}" 2>&1) || true

        if [[ "$VERBOSE" -eq 1 ]]; then
            echo "$full_output"
        fi

        # Check for compilation failure (no SCHEMATIC statistics at all)
        if ! echo "$full_output" | grep -q "SCHEMATIC Checkpoint Insertion Statistics"; then
            echo "  FAILED (compilation error)"
            echo "$row_name,$cap_label,FAILED,,,,,,,,,,,,,," >> "$OUTPUT_CSV"
            continue
        fi

        basic_blocks=$(extract_stat "$full_output" "Basic blocks")
        edges=$(extract_stat "$full_output" "Edges")
        candidate_globals=$(extract_stat "$full_output" "Candidate globals (V_elig)")
        regions=$(extract_stat "$full_output" "Regions")
        checkpoints_placed=$(extract_stat "$full_output" "Checkpoints placed")
        paths_analyzed=$(extract_stat "$full_output" "Paths analyzed")
        vm_variables=$(extract_stat "$full_output" "Variables placed in VM")
        nvm_variables=$(extract_stat "$full_output" "Variables placed in NVM")
        total_exec_time=$(extract_stat "$full_output" "Total execution time (ms)")

        # Extract mock counter stats
        prologue=$(extract_stat "$full_output" "__region_prologue")
        epilogue=$(extract_stat "$full_output" "__region_epilogue")
        store_reg=$(extract_stat "$full_output" "__checkpoint_store_reg")
        store_mem=$(extract_stat "$full_output" "__checkpoint_store_mem")
        restore_reg=$(extract_stat "$full_output" "__restore_reg")
        restore_mem=$(extract_stat "$full_output" "__restore_mem")

        # Default to 0 if not found
        prologue=${prologue:-0}
        epilogue=${epilogue:-0}
        store_reg=${store_reg:-0}
        store_mem=${store_mem:-0}
        restore_reg=${restore_reg:-0}
        restore_mem=${restore_mem:-0}
        total_exec_time=${total_exec_time:-0}

        echo "$row_name,$cap_label,$basic_blocks,$edges,$candidate_globals,$regions,$checkpoints_placed,$paths_analyzed,$vm_variables,$nvm_variables,$total_exec_time,$prologue,$epilogue,$store_reg,$store_mem,$restore_reg,$restore_mem" >> "$OUTPUT_CSV"
        echo "  OK ($regions regions, $checkpoints_placed checkpoints)"
    done
done

echo ""
echo "=========================================="
echo "Results written to: $OUTPUT_CSV"
echo "=========================================="
echo ""
column -t -s',' "$OUTPUT_CSV"

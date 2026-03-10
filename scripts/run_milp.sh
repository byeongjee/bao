#!/usr/bin/env bash
#
# Benchmark MILP checkpoint insertion across all intermittent benchmarks
# and capacitor sizes. Outputs a CSV summary.
#
# Usage:
#   ./scripts/run_milp.sh [--cap <size>] [-o output.csv] [-v|--verbose] [bench1 bench2 ...]
#
# If benchmark names are given, only those are run (matched by filename without .c).
# Example: ./scripts/run_milp.sh crc chacha20 rsa
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

OUTPUT_CSV="$PROJECT_DIR/benchmarks/milp_benchmark_summary.csv"
VERBOSE=0
FILTER_BENCHMARKS=()
FILTER_CAPS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        -o) OUTPUT_CSV="$2"; shift 2 ;;
        -v|--verbose) VERBOSE=1; shift ;;
        --cap) FILTER_CAPS+=("$2"); shift 2 ;;
        -h|--help) echo "Usage: $0 [--cap <size>] [-o output.csv] [-v|--verbose] [bench1 bench2 ...]"; exit 0 ;;
        -*) echo "Unknown option: $1" >&2; exit 1 ;;
        *) FILTER_BENCHMARKS+=("$1"); shift ;;
    esac
done

ENERGY_CONFIG="$PROJECT_DIR/benchmarks/sample_energy_config_ir.json"

ALL_CAPACITOR_CONFIGS=(
    "1uF:$PROJECT_DIR/benchmarks/sample_milp_config_1uF.json"
    "10uF:$PROJECT_DIR/benchmarks/sample_milp_config_10uF.json"
    "100uF:$PROJECT_DIR/benchmarks/sample_milp_config_100uF.json"
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
HEADER="benchmark,capacitor,status,basic_blocks,edges,regions,compilation_time_ms,peak_rss_kb,profiling_time_ms,execution_time_ms,runtime_region_prologue_calls,runtime_region_epilogue_calls,runtime_checkpoint_store_reg_calls,runtime_checkpoint_store_mem_calls,runtime_restore_reg_calls,runtime_restore_mem_calls,candidate_globals,milp_variables,milp_constraints,optimal_solution,region_boundaries_inserted,distributed_checkpoints_inserted,milp_solve_time_ms"
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
            "$SCRIPT_DIR/compile_and_run.sh"
            --mode milp
            --runtime mock-counter
            -e "$ENERGY_CONFIG"
            -m "$cap_config"
            --local
            -I "$PROJECT_DIR/passes/runtime"
            --verbose
            "$bench_path"
        )
        printf -v run_cmd_display '%q ' "${run_cmd[@]}"
        run_cmd_display="${run_cmd_display% }"
        echo "  Command: $run_cmd_display"

        # Run compile_and_run, capture all output regardless of exit code
        full_output=$("${run_cmd[@]}" 2>&1) || true

        if [[ "$VERBOSE" -eq 1 ]]; then
            echo "$full_output"
        fi

        # Check for infeasibility
        if echo "$full_output" | grep -q "blocks exceed energy capacity"; then
            echo "  INFEASIBLE (blocks exceed capacity)"
            echo "$row_name,$cap_label,infeasible,,,,,,,,,,,,,,,,,,,,," >> "$OUTPUT_CSV"
            continue
        fi
        if echo "$full_output" | grep -q "Optimization failed"; then
            echo "  INFEASIBLE (solver found no feasible solution)"
            echo "$row_name,$cap_label,infeasible,,,,,,,,,,,,,,,,,,,,," >> "$OUTPUT_CSV"
            continue
        fi

        # Check for compilation failure (no MILP statistics at all)
        if ! echo "$full_output" | grep -q "Checkpoint Insertion Statistics"; then
            echo "  FAILED (compilation error)"
            echo "$row_name,$cap_label,failed,,,,,,,,,,,,,,,,,,,,," >> "$OUTPUT_CSV"
            continue
        fi

        basic_blocks=$(extract_stat "$full_output" "Basic blocks (concrete)" "Basic blocks")
        edges=$(extract_stat "$full_output" "Edges (concrete)" "Edges")
        candidate_globals=$(extract_stat "$full_output" "Candidate globals (V_elig)")
        milp_vars=$(extract_stat "$full_output" "MILP variables")
        milp_constrs=$(extract_stat "$full_output" "MILP constraints")
        optimal_raw=$(extract_stat "$full_output" "Optimal solution")
        if [[ "$optimal_raw" == "yes" ]]; then
            optimal="yes"
        else
            optimal="no"
        fi
        regions=$(extract_stat "$full_output" "Regions")
        boundaries=$(extract_stat "$full_output" "Region boundaries inserted")
        dist_ckpts=$(extract_stat "$full_output" \
            "Distributed checkpoints inserted" \
            "Boundary commits enabled")
        solve_time=$(extract_stat "$full_output" "Solve time (ms)")
        compilation_time=$(extract_stat "$full_output" "Compilation time (ms)")
        peak_rss=$(extract_stat "$full_output" "Peak RSS (KB)")
        profiling_time=$(extract_stat "$full_output" "Profiling time (ms)")
        execution_time=$(extract_stat "$full_output" "Execution time (ms)")

        # Check for infeasible blocks
        if echo "$full_output" | grep -q "blocks exceed energy capacity"; then
            optimal="infeasible"
            regions=${regions:-0}
            boundaries=${boundaries:-0}
            dist_ckpts=${dist_ckpts:-0}
        fi

        # Defaults
        compilation_time=${compilation_time:-0}
        peak_rss=${peak_rss:-0}
        profiling_time=${profiling_time:-0}
        execution_time=${execution_time:-0}

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
        solve_time=${solve_time:-0}

        echo "$row_name,$cap_label,ok,$basic_blocks,$edges,$regions,$compilation_time,$peak_rss,$profiling_time,$execution_time,$prologue,$epilogue,$store_reg,$store_mem,$restore_reg,$restore_mem,$candidate_globals,$milp_vars,$milp_constrs,$optimal,$boundaries,$dist_ckpts,$solve_time" >> "$OUTPUT_CSV"
        echo "  OK ($regions regions, $boundaries boundaries, $optimal)"
    done
done

echo ""
echo "=========================================="
echo "Results written to: $OUTPUT_CSV"
echo "=========================================="
echo ""
column -t -s',' "$OUTPUT_CSV"

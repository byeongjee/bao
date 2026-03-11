#!/usr/bin/env bash
#
# Benchmark machine-level RockClimb checkpoint insertion across all intermittent
# benchmarks and capacitor sizes. Compiles via compile_rockclimb.sh,
# flashes to MSP430 via mspdebug, and reads runtime counters over UART.
#
# Outputs a CSV summary.
#
# Usage:
#   ./scripts/run_rockclimb.sh [--debug-counters] [--cap <size>] [-o output.csv] [-v|--verbose] [bench1 bench2 ...]
#
# Options:
#   --debug-counters  Link debug counter runtime (UART output + NVM counters).
#               Default: real runtime only (boot.S save+halt, FRAM recovery).
#   --cap <size>      Run only the given capacitor size (1uF, 10uF, 100uF).
#                     Can be repeated: --cap 1uF --cap 10uF
#
# If benchmark names are given, only those are run (matched by filename without .c).
# Example: ./scripts/run_rockclimb.sh crc chacha20 rsa
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
source "$SCRIPT_DIR/lib/common.sh"

OUTPUT_CSV="$PROJECT_DIR/benchmarks/rockclimb_benchmark_summary.csv"
VERBOSE=0
DEBUG_COUNTERS=0
FILTER_BENCHMARKS=()
FILTER_CAPS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        -o) OUTPUT_CSV="$2"; shift 2 ;;
        -v|--verbose) VERBOSE=1; shift ;;
        --debug-counters) DEBUG_COUNTERS=1; shift ;;
        --cap) FILTER_CAPS+=("$2"); shift 2 ;;
        -h|--help) echo "Usage: $0 [--debug-counters] [--cap <size>] [-o output.csv] [-v|--verbose] [bench1 bench2 ...]"; exit 0 ;;
        -*) echo "Unknown option: $1" >&2; exit 1 ;;
        *) FILTER_BENCHMARKS+=("$1"); shift ;;
    esac
done

ENERGY_CONFIG="$PROJECT_DIR/benchmarks/sample_assembly_energy_params.json"

ALL_CAPACITOR_CONFIGS=(
    "1uF:$PROJECT_DIR/benchmarks/sample_rockclimb_config_1uF.json"
    "10uF:$PROJECT_DIR/benchmarks/sample_rockclimb_config_10uF.json"
    "100uF:$PROJECT_DIR/benchmarks/sample_rockclimb_config_100uF.json"
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

# Check for MSP430 device (needed for NVM readback with --debug-counters)
HAS_DEVICE=0
if [[ "$DEBUG_COUNTERS" -eq 1 ]]; then
    if timeout 3 mspdebug tilib "exit" &>/dev/null; then
        HAS_DEVICE=1
        echo "MSP430 device detected — will flash and read NVM counters."
    else
        echo "Warning: No MSP430 device detected — skipping NVM readback (runtime counters will be 0)."
    fi
fi

# CSV header
HEADER="benchmark,capacitor,status,basic_blocks,edges,regions,compilation_time_ms,peak_rss_kb,profiling_time_ms,execution_time_ms,boundary_checks,runtime_region_boundary_calls,runtime_debug_save_reg_calls,runtime_debug_restore_reg_calls,result"
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
    # No match — caller uses ${var:-0} defaults; return 0 to avoid set -e exit.
    return 0
}

total=$((${#BENCHMARKS[@]} * ${#CAPACITOR_CONFIGS[@]}))
count=0

ALL_TMP_DIRS=()
cleanup_all() { rm -rf "${ALL_TMP_DIRS[@]}"; }
trap cleanup_all EXIT

for bench_path in "${BENCHMARKS[@]}"; do
    bench_name=$(basename "$bench_path" .c)

    for cap_entry in "${CAPACITOR_CONFIGS[@]}"; do
        cap_label="${cap_entry%%:*}"
        cap_config="${cap_entry#*:}"
        count=$((count + 1))

        row_name="${bench_name}-${cap_label}"
        echo "[$count/$total] Running $row_name ..."

        TMP_DIR=$(mktemp -d)
        ALL_TMP_DIRS+=("$TMP_DIR")

        run_cmd=(
            "$SCRIPT_DIR/compile_rockclimb.sh"
            -e "$ENERGY_CONFIG"
            -c "$cap_config"
            -o "$TMP_DIR/$bench_name"
            --verbose
            --link
        )
        if [[ "$DEBUG_COUNTERS" -eq 1 ]]; then
            run_cmd+=(--debug-counters)
        fi
        run_cmd+=("$bench_path")
        printf -v run_cmd_display '%q ' "${run_cmd[@]}"
        run_cmd_display="${run_cmd_display% }"
        echo "  Command: $run_cmd_display"

        # Run compile, capture all output regardless of exit code
        compile_output=$("${run_cmd[@]}" 2>&1) || true

        # Flash, run, and read NVM (debug-counters mode only)
        nvm_output=""
        if [[ "$HAS_DEVICE" -eq 1 && -f "$TMP_DIR/${bench_name}.elf" ]]; then
            nvm_output=$(flash_run_and_read \
                "$TMP_DIR/${bench_name}.elf" 30 \
                __nvm_done __nvm_result cnt_boundary cnt_save_reg cnt_restore_reg \
                2>"$TMP_DIR/nvm_read.err") || true
        fi

        # Convert NVM key=value output to label: value format for extract_stat
        nvm_as_labels=""
        if [[ -n "$nvm_output" ]]; then
            nvm_as_labels=$(echo "$nvm_output" | sed 's/^__nvm_result=/RESULT: /; s/^cnt_boundary=/__region_boundary: /; s/^cnt_save_reg=/reg_saves: /; s/^cnt_restore_reg=/reg_restores: /')
        fi

        # Merge compile output + NVM output for stat extraction
        full_output="${compile_output}
${nvm_as_labels}"

        if [[ "$VERBOSE" -eq 1 ]]; then
            echo "$full_output"
        fi

        # Check for infeasibility
        if echo "$full_output" | grep -q "Region partitioning failed"; then
            echo "  INFEASIBLE (region partitioning failed)"
            echo "$row_name,$cap_label,infeasible,,,,,,,,,,," >> "$OUTPUT_CSV"
            continue
        fi
        if echo "$full_output" | grep -q "blocks exceed E_safe"; then
            echo "  INFEASIBLE (blocks exceed E_safe)"
            echo "$row_name,$cap_label,infeasible,,,,,,,,,,," >> "$OUTPUT_CSV"
            continue
        fi

        # Check for compilation failure (no RockClimb metrics at all)
        if ! echo "$full_output" | grep -q "Checkpoint Insertion Statistics"; then
            echo "  FAILED (compilation error)"
            echo "$row_name,$cap_label,failed,,,,,,,,,,," >> "$OUTPUT_CSV"
            continue
        fi

        # Extract pass statistics from "=== RockClimb Metrics ===" section
        basic_blocks=$(extract_stat "$full_output" "Basic blocks")
        edges=$(extract_stat "$full_output" "Edges")
        regions=$(extract_stat "$full_output" "Regions")
        boundary_checks=$(extract_stat "$full_output" "Boundary checks")
        compilation_time=$(extract_stat "$full_output" "Compilation time (ms)")
        peak_rss=$(extract_stat "$full_output" "Peak RSS (KB)")
        execution_time=$(extract_stat "$full_output" "Execution time (ms)")

        # Extract debug counter stats from serial output
        runtime_check=$(extract_stat "$full_output" "__region_boundary")
        runtime_save_reg=$(extract_stat "$full_output" "reg_saves")
        runtime_restore_reg=$(extract_stat "$full_output" "reg_restores")

        # Extract computation result (semantic correctness check)
        bench_result=$(extract_stat "$full_output" "RESULT")

        # Default to 0 if not found
        basic_blocks=${basic_blocks:-0}
        edges=${edges:-0}
        regions=${regions:-0}
        boundary_checks=${boundary_checks:-0}
        compilation_time=${compilation_time:-0}
        peak_rss=${peak_rss:-0}
        execution_time=${execution_time:-0}
        runtime_check=${runtime_check:-0}
        runtime_save_reg=${runtime_save_reg:-0}
        runtime_restore_reg=${runtime_restore_reg:-0}
        bench_result=${bench_result:-}

        echo "$row_name,$cap_label,ok,$basic_blocks,$edges,$regions,$compilation_time,$peak_rss,,$execution_time,$boundary_checks,$runtime_check,$runtime_save_reg,$runtime_restore_reg,$bench_result" >> "$OUTPUT_CSV"
        echo "  OK ($regions regions, $boundary_checks boundaries)"
    done
done

echo ""
echo "=========================================="
echo "Results written to: $OUTPUT_CSV"
echo "=========================================="
echo ""
column -t -s',' "$OUTPUT_CSV"

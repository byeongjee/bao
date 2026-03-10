#!/usr/bin/env bash
#
# Benchmark machine-level RockClimb checkpoint insertion across all intermittent
# benchmarks and capacitor sizes. Compiles via compile_rockclimb.sh,
# flashes to MSP430 via mspdebug, and reads runtime counters over UART.
#
# Outputs a CSV summary.
#
# Usage:
#   ./scripts/run_rockclimb.sh [-o output.csv] [-v|--verbose] [bench1 bench2 ...]
#
# If benchmark names are given, only those are run (matched by filename without .c).
# Example: ./scripts/run_rockclimb.sh crc chacha20 rsa
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

OUTPUT_CSV="$PROJECT_DIR/benchmarks/rockclimb_benchmark_summary.csv"
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

ENERGY_CONFIG="$PROJECT_DIR/benchmarks/sample_assembly_energy_params.json"

CAPACITOR_CONFIGS=(
    "100nF:$PROJECT_DIR/benchmarks/sample_rockclimb_config_100nF.json"
    "1uF:$PROJECT_DIR/benchmarks/sample_rockclimb_config_1uF.json"
    "10uF:$PROJECT_DIR/benchmarks/sample_rockclimb_config_10uF.json"
    "100uF:$PROJECT_DIR/benchmarks/sample_rockclimb_config_100uF.json"
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
HEADER="benchmark,capacitor,status,basic_blocks,edges,regions,compilation_time_ms,peak_rss_kb,profiling_time_ms,execution_time_ms,runtime_region_prologue_calls,runtime_region_epilogue_calls,runtime_checkpoint_store_reg_calls,runtime_checkpoint_store_mem_calls,runtime_restore_reg_calls,runtime_restore_mem_calls,boundary_checks,register_checkpoints,avg_region_energy,max_region_energy,runtime_rockclimb_check_calls,runtime_rockclimb_save_reg_calls,runtime_rockclimb_init_calls,runtime_rockclimb_is_recovery_calls,runtime_rockclimb_recover_calls"
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

        TMP_DIR=$(mktemp -d)
        trap "rm -rf $TMP_DIR" EXIT

        run_cmd=(
            "$SCRIPT_DIR/compile_rockclimb.sh"
            -e "$ENERGY_CONFIG"
            -c "$cap_config"
            -o "$TMP_DIR/$bench_name"
            --verbose
            --link --mock-counter
            "$bench_path"
        )
        printf -v run_cmd_display '%q ' "${run_cmd[@]}"
        run_cmd_display="${run_cmd_display% }"
        echo "  Command: $run_cmd_display"

        # Run compile, capture all output regardless of exit code
        compile_output=$("${run_cmd[@]}" 2>&1) || true

        # Flash via mspdebug
        flash_output=""
        if [[ -f "$TMP_DIR/${bench_name}.elf" ]]; then
            flash_output=$(mspdebug tilib "prog $TMP_DIR/${bench_name}.elf" 2>&1) || true
        fi

        # Read serial output with timeout (program runs and prints counter summary)
        serial_output=""
        SERIAL_DEV=$(ls /dev/tty.usbmodem* 2>/dev/null | head -1)
        if [[ -z "$SERIAL_DEV" ]]; then
            SERIAL_DEV=$(ls /dev/ttyACM* 2>/dev/null | head -1)
        fi
        if [[ -n "$SERIAL_DEV" && -f "$TMP_DIR/${bench_name}.elf" ]]; then
            # Configure serial port
            stty -f "$SERIAL_DEV" 9600 cs8 -cstopb -parenb raw 2>/dev/null || \
                stty -F "$SERIAL_DEV" 9600 cs8 -cstopb -parenb raw 2>/dev/null || true
            # Read with timeout
            timeout 30 cat "$SERIAL_DEV" > "$TMP_DIR/serial.out" 2>/dev/null &
            SERIAL_PID=$!
            sleep 1
            # Reset device to start execution
            mspdebug tilib "reset" "run" 2>/dev/null &
            # Wait for output or timeout
            wait $SERIAL_PID 2>/dev/null || true
            serial_output=$(cat "$TMP_DIR/serial.out" 2>/dev/null) || true
        fi

        # Merge compile stderr + serial output for stat extraction
        full_output="${compile_output}
${serial_output}"

        if [[ "$VERBOSE" -eq 1 ]]; then
            echo "$full_output"
        fi

        # Check for infeasibility
        if echo "$full_output" | grep -q "Region partitioning failed"; then
            echo "  INFEASIBLE (region partitioning failed)"
            echo "$row_name,$cap_label,infeasible,,,,,,,,,,,,,,,,,,,,,,,," >> "$OUTPUT_CSV"
            continue
        fi
        if echo "$full_output" | grep -q "blocks exceed E_safe"; then
            echo "  INFEASIBLE (blocks exceed E_safe)"
            echo "$row_name,$cap_label,infeasible,,,,,,,,,,,,,,,,,,,,,,,," >> "$OUTPUT_CSV"
            continue
        fi

        # Check for compilation failure (no RockClimb metrics at all)
        if ! echo "$full_output" | grep -q "Checkpoint Insertion Statistics"; then
            echo "  FAILED (compilation error)"
            echo "$row_name,$cap_label,failed,,,,,,,,,,,,,,,,,,,,,,,," >> "$OUTPUT_CSV"
            continue
        fi

        # Extract pass statistics from "=== RockClimb Metrics ===" section
        basic_blocks=$(extract_stat "$full_output" "Basic blocks")
        edges=$(extract_stat "$full_output" "Edges")
        regions=$(extract_stat "$full_output" "Regions")
        boundary_checks=$(extract_stat "$full_output" "Boundary checks")
        register_ckpts=$(extract_stat "$full_output" "Register checkpoints")
        avg_region_energy=$(extract_stat "$full_output" "Avg region energy")
        max_region_energy=$(extract_stat "$full_output" "Max region energy")
        compilation_time=$(extract_stat "$full_output" "Compilation time (ms)")
        peak_rss=$(extract_stat "$full_output" "Peak RSS (KB)")
        execution_time=$(extract_stat "$full_output" "Execution time (ms)")

        # Extract mock counter stats from serial output
        # (same format as "=== RockClimb Checkpoint Counter Summary ===")
        runtime_check=$(extract_stat "$full_output" "__rockclimb_check")
        runtime_save_reg=$(extract_stat "$full_output" "__rockclimb_save_reg")
        runtime_prologue=$(extract_stat "$full_output" "__region_prologue")
        runtime_epilogue=$(extract_stat "$full_output" "__region_epilogue")
        runtime_store_reg=$(extract_stat "$full_output" "__checkpoint_store_reg")
        runtime_init=$(extract_stat "$full_output" "__rockclimb_init")
        runtime_is_recovery=$(extract_stat "$full_output" "__rockclimb_is_recovery")
        runtime_recover=$(extract_stat "$full_output" "__rockclimb_recover")

        # Default to 0 if not found
        basic_blocks=${basic_blocks:-0}
        edges=${edges:-0}
        regions=${regions:-0}
        boundary_checks=${boundary_checks:-0}
        register_ckpts=${register_ckpts:-0}
        avg_region_energy=${avg_region_energy:-0}
        max_region_energy=${max_region_energy:-0}
        compilation_time=${compilation_time:-0}
        peak_rss=${peak_rss:-0}
        execution_time=${execution_time:-0}
        runtime_check=${runtime_check:-0}
        runtime_save_reg=${runtime_save_reg:-0}
        runtime_prologue=${runtime_prologue:-0}
        runtime_epilogue=${runtime_epilogue:-0}
        runtime_store_reg=${runtime_store_reg:-0}
        runtime_init=${runtime_init:-0}
        runtime_is_recovery=${runtime_is_recovery:-0}
        runtime_recover=${runtime_recover:-0}

        echo "$row_name,$cap_label,ok,$basic_blocks,$edges,$regions,$compilation_time,$peak_rss,,$execution_time,$runtime_prologue,$runtime_epilogue,$runtime_store_reg,,,,,$boundary_checks,$register_ckpts,$avg_region_energy,$max_region_energy,$runtime_check,$runtime_save_reg,$runtime_init,$runtime_is_recovery,$runtime_recover" >> "$OUTPUT_CSV"
        echo "  OK ($regions regions, $boundary_checks boundaries)"
    done
done

echo ""
echo "=========================================="
echo "Results written to: $OUTPUT_CSV"
echo "=========================================="
echo ""
column -t -s',' "$OUTPUT_CSV"

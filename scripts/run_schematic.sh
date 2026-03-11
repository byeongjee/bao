#!/usr/bin/env bash
#
# Benchmark SCHEMATIC checkpoint insertion across all intermittent benchmarks
# and capacitor sizes. Compiles via compile_schematic.sh, flashes to MSP430
# via mspdebug, and reads runtime counters from NVM.
#
# Outputs a CSV summary.
#
# Pipeline per benchmark:
#   1. Collect trace once via compile_schematic.sh --trace-only
#   2. Per capacitor: compile with --link --debug-counters, flash, read NVM
#
# Usage:
#   ./scripts/run_schematic.sh [--debug-counters] [--halt-mode] [--cap <size>] [-o output.csv] [-v|--verbose] [bench1 bench2 ...]
#
# Options:
#   --debug-counters  Link debug counter runtime (NVM counters + UART output).
#   --halt-mode       Enable LPM4 halt at region boundaries.
#   --cap <size>      Run only the given capacitor size (1uF, 10uF, 100uF).
#                     Can be repeated: --cap 1uF --cap 10uF
#
# If benchmark names are given, only those are run (matched by filename without .c).
# Example: ./scripts/run_schematic.sh --debug-counters sha256 aes
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
source "$SCRIPT_DIR/lib/common.sh"

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

# CSV header — shared columns match RockClimb, then SCHEMATIC-specific columns
HEADER="benchmark,capacitor,status,basic_blocks,edges,regions,compilation_time_ms,peak_rss_kb,profiling_time_ms,runtime_region_boundary_calls,runtime_save_reg_calls,runtime_restore_reg_calls,runtime_store_mem_calls,runtime_restore_mem_calls,result,candidate_globals,region_boundaries,enabled_checkpoints,loop_decisions,paths_analyzed,runtime_calls_inserted"
echo "$HEADER" > "$OUTPUT_CSV"

FAIL_COLS=",,,,,,,,,,,,,,,,,,,"  # 19 empty fields for error rows

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
    # No match — caller uses ${var:-0} defaults; return 0 to avoid set -e exit.
    return 0
}

TMPDIR="${TMPDIR:-/tmp}/schematic_bench_$$"
mkdir -p "$TMPDIR"
trap "rm -rf $TMPDIR" EXIT

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
        $VERBOSE_FLAG \
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

    # === Per-capacitor: compile, flash, read NVM ===
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

        # Flash, run, and read NVM (debug-counters mode only, requires device)
        nvm_output=""
        if [[ "$HAS_DEVICE" -eq 1 && -f "$TMPDIR/${bench_name}.elf" ]]; then
            nvm_output=$(flash_run_and_read \
                "$TMPDIR/${bench_name}.elf" 30 \
                __nvm_done __nvm_result cnt_boundary cnt_save_reg cnt_restore_reg cnt_store_mem cnt_restore_mem \
                2>"$TMPDIR/nvm_read.err") || true
        fi

        # Convert NVM key=value output to label: value format for extract_stat
        nvm_as_labels=""
        if [[ -n "$nvm_output" ]]; then
            nvm_as_labels=$(echo "$nvm_output" | sed \
                -e 's/^__nvm_result=/RESULT: /' \
                -e 's/^cnt_boundary=/__region_boundary: /' \
                -e 's/^cnt_save_reg=/reg_saves: /' \
                -e 's/^cnt_restore_reg=/reg_restores: /' \
                -e 's/^cnt_store_mem=/mem_stores: /' \
                -e 's/^cnt_restore_mem=/mem_restores: /')
        fi

        # Merge compile output + NVM output for stat extraction
        full_output="${compile_output}
${nvm_as_labels}"

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

        # Extract pass statistics
        basic_blocks=$(extract_stat "$full_output" "Basic blocks")
        edges=$(extract_stat "$full_output" "Edges")
        regions=$(extract_stat "$full_output" "Regions")
        compilation_time=$(extract_stat "$full_output" "Compilation time (ms)")
        peak_rss=$(extract_stat "$full_output" "Peak RSS (KB)")

        # SCHEMATIC-specific pass stats
        candidate_globals=$(extract_stat "$full_output" "Candidate globals (V_elig)")
        region_boundaries=$(extract_stat "$full_output" "Region boundaries")
        enabled_ckpts=$(extract_stat "$full_output" "Enabled checkpoints")
        loop_decisions=$(extract_stat "$full_output" "Loop decisions")
        paths_analyzed=$(extract_stat "$full_output" "Paths analyzed")
        runtime_calls=$(extract_stat "$full_output" "Runtime calls inserted")

        # Extract runtime counters from NVM readback (device only)
        rt_boundary=$(extract_stat "$full_output" "__region_boundary")
        rt_save_reg=$(extract_stat "$full_output" "reg_saves")
        rt_restore_reg=$(extract_stat "$full_output" "reg_restores")
        rt_store_mem=$(extract_stat "$full_output" "mem_stores")
        rt_restore_mem=$(extract_stat "$full_output" "mem_restores")

        # Extract computation result (semantic correctness check)
        bench_result=$(extract_stat "$full_output" "RESULT")

        # Default to 0/empty if not found
        basic_blocks=${basic_blocks:-0}
        edges=${edges:-0}
        regions=${regions:-0}
        compilation_time=${compilation_time:-0}
        peak_rss=${peak_rss:-0}
        candidate_globals=${candidate_globals:-0}
        region_boundaries=${region_boundaries:-0}
        enabled_ckpts=${enabled_ckpts:-0}
        loop_decisions=${loop_decisions:-0}
        paths_analyzed=${paths_analyzed:-0}
        runtime_calls=${runtime_calls:-0}
        rt_boundary=${rt_boundary:-0}
        rt_save_reg=${rt_save_reg:-0}
        rt_restore_reg=${rt_restore_reg:-0}
        rt_store_mem=${rt_store_mem:-0}
        rt_restore_mem=${rt_restore_mem:-0}
        bench_result=${bench_result:-}

        echo "$row_name,$cap_label,ok,$basic_blocks,$edges,$regions,$compilation_time,$peak_rss,$PROFILING_TIME_MS,$rt_boundary,$rt_save_reg,$rt_restore_reg,$rt_store_mem,$rt_restore_mem,$bench_result,$candidate_globals,$region_boundaries,$enabled_ckpts,$loop_decisions,$paths_analyzed,$runtime_calls" >> "$OUTPUT_CSV"
        echo "  OK ($regions regions, $enabled_ckpts checkpoints, $runtime_calls runtime calls, $rt_boundary boundaries)"
    done
done

echo ""
echo "=========================================="
echo "Results written to: $OUTPUT_CSV"
echo "=========================================="
echo ""
column -t -s',' "$OUTPUT_CSV"

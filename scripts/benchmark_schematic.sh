#!/usr/bin/env bash
#
# Benchmark SCHEMATIC checkpoint insertion across all intermittent benchmarks
# and capacitor sizes. Outputs a CSV summary.
#
# Pipeline per benchmark:
#   1. clang -O0 → LLVM IR
#   2. opt -passes=tripcount-annotation → annotate loop trip counts
#   3. opt -O2 → optimize
#   4. opt -passes=trace-collect → instrument for tracing
#   5. compile + run trace binary → schematic_trace.json  (once per benchmark)
#   6. opt -passes="tripcount-annotation,schematic" → checkpoint insertion (per capacitor)
#   7. compile + run instrumented binary with mock counter runtime (per capacitor)
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

# Paths
PASS_PLUGIN="$PROJECT_DIR/passes/build/CheckpointPass.so"
ENERGY_CONFIG="$PROJECT_DIR/benchmarks/sample_energy_config_ir.json"
TRACE_RUNTIME="$PROJECT_DIR/passes/runtime/schematic_trace_runtime.c"
MOCK_RUNTIME="$PROJECT_DIR/passes/runtime/schematic_mock_ckpt_counter.c"
RUNTIME_DIR="$PROJECT_DIR/passes/runtime"
TMPDIR="${TMPDIR:-/tmp}/schematic_bench_$$"
mkdir -p "$TMPDIR"

# macOS SDK for system headers (custom-built clang needs this)
SDK_PATH=""
if [[ "$(uname)" == "Darwin" ]]; then
    SDK_PATH=$(xcrun --show-sdk-path 2>/dev/null || true)
fi
SDK_FLAGS=()
if [[ -n "$SDK_PATH" ]]; then
    SDK_FLAGS=(-isysroot "$SDK_PATH")
fi

_now_ms() { python3 -c 'import time; print(int(time.time() * 1000))'; }

CAPACITOR_CONFIGS=(
    "100nF:$PROJECT_DIR/benchmarks/sample_schematic_config_100nF.json"
    "1uF:$PROJECT_DIR/benchmarks/sample_schematic_config_1uF.json"
    "10uF:$PROJECT_DIR/benchmarks/sample_schematic_config_10uF.json"
    "100uF:$PROJECT_DIR/benchmarks/sample_schematic_config_100uF.json"
)

# Verify pass plugin exists
if [[ ! -f "$PASS_PLUGIN" ]]; then
    echo "Error: Pass plugin not found at $PASS_PLUGIN" >&2
    echo "       Run: cd passes/build && cmake .. && make" >&2
    exit 1
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

total=$((${#BENCHMARKS[@]} * ${#CAPACITOR_CONFIGS[@]}))
count=0

for bench_path in "${BENCHMARKS[@]}"; do
    bench_name=$(basename "$bench_path" .c)

    # === Per-benchmark preparation (steps 1-5): compile, annotate, optimize, trace ===
    echo ""
    echo "--- Preparing $bench_name (compile + trace) ---"

    ll_o0="$TMPDIR/${bench_name}_O0.ll"
    ll_ann="$TMPDIR/${bench_name}_ann.ll"
    ll_o2="$TMPDIR/${bench_name}_O2.ll"
    ll_trace_inst="$TMPDIR/${bench_name}_trace_inst.ll"
    trace_bin="$TMPDIR/${bench_name}_trace_run"
    trace_json="$TMPDIR/${bench_name}_trace.json"

    # Step 1: C → LLVM IR at O0
    prep_output=""
    if ! prep_output=$(clang -S -emit-llvm -O0 -Xclang -disable-O0-optnone \
            -I "$RUNTIME_DIR" "${SDK_FLAGS[@]}" \
            "$bench_path" -o "$ll_o0" 2>&1); then
        echo "  FAILED: clang compilation"
        [[ "$VERBOSE" -eq 1 ]] && echo "$prep_output"
        for cap_entry in "${CAPACITOR_CONFIGS[@]}"; do
            cap_label="${cap_entry%%:*}"
            count=$((count + 1))
            echo "${bench_name}-${cap_label},${cap_label},COMPILE_FAILED${FAIL_COLS}" >> "$OUTPUT_CSV"
        done
        continue
    fi

    # Step 2: Annotate trip counts
    if ! prep_output=$(opt -load-pass-plugin="$PASS_PLUGIN" \
            -passes=tripcount-annotation \
            -S "$ll_o0" -o "$ll_ann" 2>&1); then
        echo "  FAILED: trip count annotation"
        [[ "$VERBOSE" -eq 1 ]] && echo "$prep_output"
        for cap_entry in "${CAPACITOR_CONFIGS[@]}"; do
            cap_label="${cap_entry%%:*}"
            count=$((count + 1))
            echo "${bench_name}-${cap_label},${cap_label},ANNOTATE_FAILED${FAIL_COLS}" >> "$OUTPUT_CSV"
        done
        continue
    fi

    # Step 3: Optimize at O2
    if ! prep_output=$(opt -O2 -S "$ll_ann" -o "$ll_o2" 2>&1); then
        echo "  FAILED: O2 optimization"
        [[ "$VERBOSE" -eq 1 ]] && echo "$prep_output"
        for cap_entry in "${CAPACITOR_CONFIGS[@]}"; do
            cap_label="${cap_entry%%:*}"
            count=$((count + 1))
            echo "${bench_name}-${cap_label},${cap_label},OPT_FAILED${FAIL_COLS}" >> "$OUTPUT_CSV"
        done
        continue
    fi

    # Step 4-5: Trace collection (timed for profiling_time_ms)
    PROFILE_START=$(_now_ms)

    # Step 4: Instrument for trace collection
    if ! prep_output=$(opt -load-pass-plugin="$PASS_PLUGIN" \
            -passes=trace-collect \
            -energy-config="$ENERGY_CONFIG" \
            -S "$ll_o2" -o "$ll_trace_inst" 2>&1); then
        echo "  FAILED: trace instrumentation"
        [[ "$VERBOSE" -eq 1 ]] && echo "$prep_output"
        for cap_entry in "${CAPACITOR_CONFIGS[@]}"; do
            cap_label="${cap_entry%%:*}"
            count=$((count + 1))
            echo "${bench_name}-${cap_label},${cap_label},TRACE_INST_FAILED${FAIL_COLS}" >> "$OUTPUT_CSV"
        done
        continue
    fi
    [[ "$VERBOSE" -eq 1 ]] && echo "$prep_output"

    # Step 5: Compile and run trace binary
    if ! prep_output=$(clang "${SDK_FLAGS[@]}" \
            "$ll_trace_inst" "$TRACE_RUNTIME" \
            -o "$trace_bin" 2>&1); then
        echo "  FAILED: trace binary compilation"
        [[ "$VERBOSE" -eq 1 ]] && echo "$prep_output"
        for cap_entry in "${CAPACITOR_CONFIGS[@]}"; do
            cap_label="${cap_entry%%:*}"
            count=$((count + 1))
            echo "${bench_name}-${cap_label},${cap_label},TRACE_COMPILE_FAILED${FAIL_COLS}" >> "$OUTPUT_CSV"
        done
        continue
    fi

    # Run trace binary (ignore exit code — benchmarks may return non-zero).
    # Keep output on disk to avoid exploding shell memory on noisy runs.
    trace_run_log="$TMPDIR/${bench_name}_trace_run.log"
    if [[ "$VERBOSE" -eq 1 ]]; then
        "$trace_bin" 2>&1 | tee "$trace_run_log" || true
    else
        "$trace_bin" > "$trace_run_log" 2>&1 || true
    fi

    # The trace runtime writes to schematic_trace.json in CWD
    if [[ -f "schematic_trace.json" ]]; then
        mv "schematic_trace.json" "$trace_json"
    else
        echo "  FAILED: trace binary did not produce schematic_trace.json"
        [[ "$VERBOSE" -eq 0 ]] && tail -20 "$trace_run_log"
        for cap_entry in "${CAPACITOR_CONFIGS[@]}"; do
            cap_label="${cap_entry%%:*}"
            count=$((count + 1))
            echo "${bench_name}-${cap_label},${cap_label},TRACE_RUN_FAILED${FAIL_COLS}" >> "$OUTPUT_CSV"
        done
        continue
    fi

    PROFILE_END=$(_now_ms)
    PROFILING_TIME_MS=$((PROFILE_END - PROFILE_START))
    echo "  Trace collected for $bench_name (profiling: ${PROFILING_TIME_MS}ms)"

    # === Per-capacitor: run SCHEMATIC (step 6) + compile & run (step 7) ===
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

        # Step 6: Run SCHEMATIC pass
        ll_out="$TMPDIR/${bench_name}_${cap_label}_out.ll"
        full_output=$(opt -load-pass-plugin="$PASS_PLUGIN" \
            -passes="tripcount-annotation,schematic" \
            -energy-config="$ENERGY_CONFIG" \
            -schematic-config="$cap_config" \
            -schematic-trace="$trace_json" \
            -S "$ll_o2" -o "$ll_out" 2>&1) || true

        if [[ "$VERBOSE" -eq 1 ]]; then
            echo "$full_output"
        fi

        # Check for failure
        if ! echo "$full_output" | grep -q "SCHEMATIC Checkpoint Insertion Statistics"; then
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

        # Step 7: Compile and run instrumented binary with mock counter
        # Strip ELF-only .nvm section specifier (invalid on Mach-O)
        sed -i '' 's/, section ".nvm"//g' "$ll_out"

        inst_bin="$TMPDIR/${bench_name}_${cap_label}_run"
        rt_prologue=""
        rt_epilogue=""
        rt_store_reg=""
        rt_store_mem=""
        rt_restore_reg=""
        rt_restore_mem=""

        if compile_output=$(clang "${SDK_FLAGS[@]}" \
                "$ll_out" "$MOCK_RUNTIME" \
                -o "$inst_bin" 2>&1); then
            # Run instrumented binary (ignore exit code)
            EXEC_START=$(_now_ms)
            run_output=$("$inst_bin" 2>&1) || true
            EXEC_END=$(_now_ms)
            execution_time_ms=$((EXEC_END - EXEC_START))
            [[ "$VERBOSE" -eq 1 ]] && echo "$run_output"

            rt_prologue=$(extract_counter "$run_output" "__region_prologue")
            rt_epilogue=$(extract_counter "$run_output" "__region_epilogue")
            rt_store_reg=$(extract_counter "$run_output" "__checkpoint_store_reg")
            rt_store_mem=$(extract_counter "$run_output" "__checkpoint_store_mem")
            rt_restore_reg=$(extract_counter "$run_output" "__restore_reg")
            rt_restore_mem=$(extract_counter "$run_output" "__restore_mem")
        else
            echo "  WARNING: instrumented binary compilation failed"
            [[ "$VERBOSE" -eq 1 ]] && echo "$compile_output"
            rt_prologue="COMPILE_FAILED"
            rt_epilogue=""
            rt_store_reg=""
            rt_store_mem=""
            rt_restore_reg=""
            rt_restore_mem=""
        fi

        echo "$row_name,$cap_label,ok,$basic_blocks,$edges,$regions,$compilation_time,$peak_rss,$PROFILING_TIME_MS,${execution_time_ms:-},$rt_prologue,$rt_epilogue,$rt_store_reg,$rt_store_mem,$rt_restore_reg,$rt_restore_mem,$candidate_globals,$enabled_ckpts,$loop_decisions,$paths_analyzed,$runtime_calls" >> "$OUTPUT_CSV"
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

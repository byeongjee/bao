#!/usr/bin/env bash
#
# Verify semantic correctness of RockClimb checkpoint insertion.
# Compiles each benchmark with and without RockClimb, flashes both to hardware,
# reads UART output, and compares RESULT values.
#
# Usage:
#   verify_rockclimb.sh [options] [bench1 bench2 ...]
#
# Options:
#   --cap <size>         Capacitor config: 1uF (default), 10uF, 100uF
#   --timeout <sec>      Serial read timeout (default: 30)
#   -v, --verbose        Show full compile/serial output
#   -h, --help           Show help
#
# Exit code: 0 if all pass, 1 if any fail.
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib/common.sh"

# Defaults
CAP_SIZE="1uF"
TIMEOUT=30
VERBOSE=0
HALT_MODE="bor"
FILTER_BENCHMARKS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --cap) CAP_SIZE="$2"; shift 2 ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        -v|--verbose) VERBOSE=1; shift ;;
        -h|--help) sed -n '2,18p' "$0" | sed 's/^# \?//'; exit 0 ;;
        -*) echo "Unknown option: $1" >&2; exit 1 ;;
        *) FILTER_BENCHMARKS+=("$1"); shift ;;
    esac
done

# Validate capacitor size
CAP_CONFIG="$PROJECT_DIR/benchmarks/sample_rockclimb_config_${CAP_SIZE}.json"
[[ ! -f "$CAP_CONFIG" ]] && { echo "Error: Config not found for cap size '$CAP_SIZE': $CAP_CONFIG" >&2; exit 1; }

ENERGY_CONFIG="$PROJECT_DIR/benchmarks/sample_assembly_energy_params.json"
[[ ! -f "$ENERGY_CONFIG" ]] && { echo "Error: Energy config not found: $ENERGY_CONFIG" >&2; exit 1; }

# Discover benchmarks
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

# ── Helpers ──────────────────────────────────────────────────────────────────

# Extract first numeric token after "label:" from output.
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

# Compile a baseline (no RockClimb) ELF for semantic comparison.
# Uses the same LLVM pipeline as compile_rockclimb.sh but without the machine pass.
compile_baseline() {
    local input="$1" output="$2"

    # Step 1: C → LLVM IR (same flags as compile_rockclimb.sh)
    $CLANG -S -emit-llvm -O2 --target=msp430 \
        -isystem "$MSP430GCC_SUPPORT_PATH/include" \
        -isystem "$MSP430GCC_SUPPORT_PATH/msp430-elf/include" \
        -I"$PROJECT_DIR/passes/runtime" \
        -DDEBUG_COUNTERS \
        "$input" -o "${output}.raw.ll"

    # Step 1b: Strip __loop_tripcount calls
    "$OPT" -load-pass-plugin="$PASS_LIB" \
        -passes=tripcount-annotation \
        -S "${output}.raw.ll" -o "${output}.ll"

    # Step 2: IR → assembly (straight llc, no machine pass)
    $LLC -march=msp430 "${output}.ll" -o "${output}.raw.s"

    # Step 3: Strip .cfi_* directives (same workaround as compile_rockclimb.sh)
    sed '/\.cfi_/d' "${output}.raw.s" > "${output}.s"

    # Step 4: Assemble
    $GCC -mmcu=$DEVICE -msmall -c "${output}.s" -o "${output}.o"

    # Step 5: Compile debug counters runtime
    $GCC -mmcu=$DEVICE -msmall -O2 -DDEBUG_COUNTERS \
        -I"$MSP430GCC_SUPPORT_PATH/include" \
        -I"$PROJECT_DIR/passes/runtime" \
        -c "$PROJECT_DIR/passes/runtime/rockclimb_debug_counters.c" \
        -o "${output}.debug_counters.o"

    # Step 6: Link (no boot.S, no rockclimb_runtime.c — baseline has no checkpoints)
    $GCC -mmcu=$DEVICE -msmall \
        -L"$MSP430GCC_SUPPORT_PATH/include" \
        -T "$PROJECT_DIR/passes/runtime/rockclimb_msp430fr5994.ld" \
        -Wl,--nmagic \
        "${output}.o" "${output}.debug_counters.o" -o "${output}.elf"
}

# Flash ELF and read serial output.
# Returns: serial output on stdout.
# Exit codes: 0=success, 1=error, 2=timeout.
flash_and_read() {
    local elf="$1" timeout="$2"

    # Flash
    mspdebug tilib "prog $elf" 2>&1 || { echo "FLASH_ERROR"; return 1; }

    # Read serial
    uv run python3 "$SCRIPT_DIR/lib/read_serial.py" \
        --timeout "$timeout" \
        --reset-cmd "mspdebug tilib \"reset\" \"run\""
    return $?
}

# ── Main loop ────────────────────────────────────────────────────────────────

PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0
ERROR_COUNT=0
RESULTS=()
total=${#BENCHMARKS[@]}
count=0

for bench_path in "${BENCHMARKS[@]}"; do
    bench_name=$(basename "$bench_path" .c)
    count=$((count + 1))

    echo "[$count/$total] $bench_name ..."

    TMP_DIR=$(mktemp -d)
    trap "rm -rf $TMP_DIR" EXIT

    # ── A: Compile baseline ──────────────────────────────────────────────
    baseline_output=""
    if compile_baseline "$bench_path" "$TMP_DIR/baseline" 2>"$TMP_DIR/baseline_compile.err"; then
        : # success
    else
        echo "  ERROR: Baseline compilation failed"
        if [[ "$VERBOSE" -eq 1 ]]; then
            cat "$TMP_DIR/baseline_compile.err"
        fi
        ERROR_COUNT=$((ERROR_COUNT + 1))
        RESULTS+=("$(printf "  %-20s ERROR (baseline compile failed)" "$bench_name")")
        continue
    fi

    # ── B: Flash + read baseline ─────────────────────────────────────────
    baseline_serial=$(flash_and_read "$TMP_DIR/baseline.elf" "$TIMEOUT" 2>"$TMP_DIR/baseline_serial.err") || true

    if [[ "$VERBOSE" -eq 1 ]]; then
        echo "  [baseline serial] $baseline_serial"
    fi

    baseline_result=$(extract_stat "$baseline_serial" "RESULT") || true
    if [[ -z "$baseline_result" ]]; then
        echo "  ERROR: No RESULT from baseline"
        ERROR_COUNT=$((ERROR_COUNT + 1))
        RESULTS+=("$(printf "  %-20s ERROR (no baseline RESULT)" "$bench_name")")
        continue
    fi

    # ── C: Compile with RockClimb ────────────────────────────────────────
    rockclimb_compile_output=$("$SCRIPT_DIR/compile_rockclimb.sh" \
        --debug-counters \
        --halt-mode "$HALT_MODE" \
        -e "$ENERGY_CONFIG" \
        -c "$CAP_CONFIG" \
        -o "$TMP_DIR/rockclimb" \
        --verbose \
        "$bench_path" 2>&1) || true

    if [[ "$VERBOSE" -eq 1 ]]; then
        echo "  [rockclimb compile] $rockclimb_compile_output"
    fi

    # ── D: Check for infeasibility ───────────────────────────────────────
    if echo "$rockclimb_compile_output" | grep -q "Region partitioning failed"; then
        echo "  SKIP (region partitioning failed)"
        SKIP_COUNT=$((SKIP_COUNT + 1))
        RESULTS+=("$(printf "  %-20s SKIP (infeasible)" "$bench_name")")
        continue
    fi
    if echo "$rockclimb_compile_output" | grep -q "blocks exceed E_safe"; then
        echo "  SKIP (blocks exceed E_safe)"
        SKIP_COUNT=$((SKIP_COUNT + 1))
        RESULTS+=("$(printf "  %-20s SKIP (infeasible)" "$bench_name")")
        continue
    fi

    # Check for compilation failure
    if [[ ! -f "$TMP_DIR/rockclimb.elf" ]]; then
        echo "  ERROR: RockClimb compilation failed (no ELF produced)"
        ERROR_COUNT=$((ERROR_COUNT + 1))
        RESULTS+=("$(printf "  %-20s ERROR (rockclimb compile failed)" "$bench_name")")
        continue
    fi

    # ── E: Flash + read RockClimb ────────────────────────────────────────
    rockclimb_serial=$(flash_and_read "$TMP_DIR/rockclimb.elf" "$TIMEOUT" 2>"$TMP_DIR/rockclimb_serial.err") || true

    if [[ "$VERBOSE" -eq 1 ]]; then
        echo "  [rockclimb serial] $rockclimb_serial"
    fi

    rockclimb_result=$(extract_stat "$rockclimb_serial" "RESULT") || true
    if [[ -z "$rockclimb_result" ]]; then
        echo "  ERROR: No RESULT from RockClimb"
        ERROR_COUNT=$((ERROR_COUNT + 1))
        RESULTS+=("$(printf "  %-20s ERROR (no rockclimb RESULT)  baseline=%s" "$bench_name" "$baseline_result")")
        continue
    fi

    # ── F: Compare results ───────────────────────────────────────────────
    if [[ "$baseline_result" == "$rockclimb_result" ]]; then
        echo "  PASS (baseline=$baseline_result rockclimb=$rockclimb_result)"
        PASS_COUNT=$((PASS_COUNT + 1))
        RESULTS+=("$(printf "  %-20s baseline=%-10s rockclimb=%-10s PASS" "$bench_name" "$baseline_result" "$rockclimb_result")")
    else
        echo "  FAIL (baseline=$baseline_result rockclimb=$rockclimb_result)"
        FAIL_COUNT=$((FAIL_COUNT + 1))
        RESULTS+=("$(printf "  %-20s baseline=%-10s rockclimb=%-10s FAIL" "$bench_name" "$baseline_result" "$rockclimb_result")")
    fi
done

# ── Summary ──────────────────────────────────────────────────────────────────

echo ""
echo "=== RockClimb Semantic Verification ==="
echo "Halt mode: $HALT_MODE | Capacitor: $CAP_SIZE"
echo ""
for line in "${RESULTS[@]}"; do
    echo "$line"
done
echo ""

passed_total=$((PASS_COUNT + SKIP_COUNT))
tested_total=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT + ERROR_COUNT))

echo "${PASS_COUNT}/${tested_total} PASSED, ${FAIL_COUNT} FAILED, ${SKIP_COUNT} SKIPPED, ${ERROR_COUNT} ERRORS"

if [[ "$FAIL_COUNT" -gt 0 || "$ERROR_COUNT" -gt 0 ]]; then
    exit 1
fi
exit 0

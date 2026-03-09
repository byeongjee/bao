#!/usr/bin/env bash
#
# Compile C source with SCHEMATIC checkpoint insertion.
#
# Usage:
#   compile_schematic.sh -e <energy_config> -s <schematic_config> [options] <input.c>
#
# Options:
#   -e <config>          Energy estimator config JSON (required)
#   -s <config>          SCHEMATIC config JSON (required)
#   -o <output>          Output base name (default: build/<input>)
#   -O <level>           LLC optimization level (default: 2)
#   -Oc <level>          Clang optimization level (default: 2)
#   -I <dir>             Add include directory (can be repeated)
#   --local              Compile for host machine instead of MSP430
#   --verbose            Show detailed pass output
#   --debug              Enable DEBUG output
#   --add-debug-markers  Insert mock-counter debug markers
#   --energy-validate    Run energy-validate pass after SCHEMATIC
#   --trace <file>       Use pre-collected trace (skip collection)
#   --trace-only         Collect trace and stop; output <output>_trace.json
#   -h, --help           Show this help message
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib/common.sh"

# Defaults
ESTIMATOR_CONFIG=""
SCHEMATIC_CONFIG=""
OUTPUT=""
OPT_LEVEL="2"
CLANG_OPT_LEVEL="2"
EXTRA_INCLUDES=""
LOCAL_MODE="false"
VERBOSE="false"
DEBUG_MODE="false"
ADD_DEBUG_MARKERS="false"
ENERGY_VALIDATE="false"
TRACE_FILE=""
TRACE_ONLY="false"
INPUT=""

usage() { sed -n '2,22p' "$0" | sed 's/^# \?//'; exit 0; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        -e|--energy-config) ESTIMATOR_CONFIG="$2"; shift 2 ;;
        -s|--schematic-config) SCHEMATIC_CONFIG="$2"; shift 2 ;;
        -o) OUTPUT="$2"; shift 2 ;;
        -O) OPT_LEVEL="$2"; shift 2 ;;
        -Oc) CLANG_OPT_LEVEL="$2"; shift 2 ;;
        -I) EXTRA_INCLUDES="$EXTRA_INCLUDES -I$2"; shift 2 ;;
        --local) LOCAL_MODE="true"; shift ;;
        --verbose) VERBOSE="true"; shift ;;
        --debug) DEBUG_MODE="true"; shift ;;
        --add-debug-markers) ADD_DEBUG_MARKERS="true"; shift ;;
        --energy-validate) ENERGY_VALIDATE="true"; shift ;;
        --trace) TRACE_FILE="$2"; shift 2 ;;
        --trace-only) TRACE_ONLY="true"; shift ;;
        -h|--help) usage ;;
        -*) error "Unknown option: $1" ;;
        *) INPUT="$1"; shift ;;
    esac
done

# Validate
[[ -z "$INPUT" ]] && error "No input file specified"
[[ ! -f "$INPUT" ]] && error "File not found: $INPUT"
[[ -z "$ESTIMATOR_CONFIG" ]] && error "Energy config required: use -e <config.json>"
[[ ! -f "$ESTIMATOR_CONFIG" ]] && error "Estimator config not found: $ESTIMATOR_CONFIG"
if [[ "$TRACE_ONLY" != "true" ]]; then
    [[ -z "$SCHEMATIC_CONFIG" ]] && error "SCHEMATIC config required: use -s <config.json>"
    [[ ! -f "$SCHEMATIC_CONFIG" ]] && error "SCHEMATIC config not found: $SCHEMATIC_CONFIG"
fi
[[ -n "$TRACE_FILE" && ! -f "$TRACE_FILE" ]] && error "Trace file not found: $TRACE_FILE"
[[ ! -f "$PASS_LIB" ]] && error "Pass not found: $PASS_LIB"

# Output name
BUILD_DIR="$PROJECT_DIR/build"
[[ -z "$OUTPUT" ]] && OUTPUT="$BUILD_DIR/$(basename "$INPUT" .c)"
mkdir -p "$(dirname "$OUTPUT")"

TMP_DIR=$(mktemp -d)
trap "rm -rf $TMP_DIR" EXIT

# Build compile_to_ir args — compile at -O0 to preserve loops for tripcount
IR_ARGS=("$INPUT" "$TMP_DIR/input.ll" --clang-opt-level 0)
[[ "$LOCAL_MODE" == "true" ]] && IR_ARGS+=(--local)
[[ "$DEBUG_MODE" == "true" ]] && IR_ARGS+=(--debug)
for word in $EXTRA_INCLUDES; do
    if [[ "$word" == -I* ]]; then
        IR_ARGS+=(-I "${word#-I}")
    fi
done

info "Compiling with SCHEMATIC..."
compile_to_ir "${IR_ARGS[@]}"

# TripCount annotation (before optimization)
if ! $OPT -load-pass-plugin="$PASS_LIB" \
    -passes=tripcount-annotation \
    -S "$TMP_DIR/input.ll" -o "$TMP_DIR/tripcount.ll"; then
    error "TripCountAnnotation pass failed"
fi

# Frontend optimization (if -Oc > 0)
SCHEMATIC_INPUT_LL="$TMP_DIR/tripcount.ll"
if [[ "$CLANG_OPT_LEVEL" != "0" ]]; then
    if ! $OPT -passes="default<O$CLANG_OPT_LEVEL>" \
        -vectorize-loops=false -vectorize-slp=false \
        -S "$TMP_DIR/tripcount.ll" -o "$TMP_DIR/input_optimized.ll"; then
        error "IR optimization pipeline failed at -O$CLANG_OPT_LEVEL"
    fi
    SCHEMATIC_INPUT_LL="$TMP_DIR/input_optimized.ll"
fi

# Trace collection (unless --trace provided)
TRACE_JSON=""
if [[ -n "$TRACE_FILE" ]]; then
    TRACE_JSON="$TRACE_FILE"
else
    PROFILE_START=$(_now_ms)
    info "Collecting execution trace..."

    if ! $OPT -load-pass-plugin="$PASS_LIB" \
        -passes=trace-collect \
        -energy-config="$ESTIMATOR_CONFIG" \
        -S "$SCHEMATIC_INPUT_LL" -o "$TMP_DIR/trace_inst.ll" 2>&1; then
        error "Trace instrumentation pass failed"
    fi

    if ! $CLANG -O0 $_SYSROOT_FLAGS \
        "$TMP_DIR/trace_inst.ll" "$SCHEMATIC_TRACE_RUNTIME" \
        -o "$TMP_DIR/trace_run" 2>&1; then
        error "Trace binary compilation failed"
    fi

    (cd "$TMP_DIR" && ./trace_run) || true

    TRACE_JSON="$TMP_DIR/schematic_trace.json"
    if [[ ! -f "$TRACE_JSON" ]]; then
        error "Trace binary did not produce schematic_trace.json"
    fi

    PROFILE_END=$(_now_ms)
    echo "Profiling time (ms): $((PROFILE_END - PROFILE_START))"
fi

# --trace-only: copy trace to output and stop
if [[ "$TRACE_ONLY" == "true" ]]; then
    cp "$TRACE_JSON" "${OUTPUT}_trace.json"
    info "Trace saved to ${OUTPUT}_trace.json"
    exit 0
fi

# SCHEMATIC pass
SCHEMATIC_EXTRA_FLAGS=""
[[ "$ADD_DEBUG_MARKERS" == "true" ]] && SCHEMATIC_EXTRA_FLAGS="-add-debug-markers"

PASS_LOG=$(mktemp "$TMP_DIR/pass_XXXXXX.log")
if $OPT -load-pass-plugin="$PASS_LIB" \
    -passes="tripcount-annotation,schematic" \
    -energy-config="$ESTIMATOR_CONFIG" \
    -schematic-config="$SCHEMATIC_CONFIG" \
    -schematic-trace="$TRACE_JSON" \
    $SCHEMATIC_EXTRA_FLAGS \
    -S "$SCHEMATIC_INPUT_LL" -o "$TMP_DIR/ckpt.ll" \
    >"$PASS_LOG" 2>&1; then
    if [[ "$VERBOSE" == "true" ]]; then
        cat "$PASS_LOG"
    else
        head -10 "$PASS_LOG"
    fi
else
    cat "$PASS_LOG" >&2
    error "SCHEMATIC pass failed (see output above)"
fi

# Energy-validate pass
if [[ "$ENERGY_VALIDATE" == "true" ]]; then
    info "Running energy-validate pass..."
    VALIDATE_FLAGS="-energy-config=$ESTIMATOR_CONFIG -schematic-config=$SCHEMATIC_CONFIG -validate-mode=schematic"
    [[ "$VERBOSE" == "true" ]] && VALIDATE_FLAGS="$VALIDATE_FLAGS -validate-verbose"
    $OPT -load-pass-plugin="$PASS_LIB" \
        -passes=energy-validate \
        $VALIDATE_FLAGS \
        -S "$TMP_DIR/ckpt.ll" -o "$TMP_DIR/ckpt.ll"
fi

# Output
if [[ "$LOCAL_MODE" == "true" ]]; then
    sed -i '' 's/, section ".nvm"//g' "$TMP_DIR/ckpt.ll"
    cp "$TMP_DIR/ckpt.ll" "${OUTPUT}.ll"
else
    $LLC -march=msp430 -O"$OPT_LEVEL" "$TMP_DIR/ckpt.ll" -o "$TMP_DIR/ckpt.s"
    $GCC -mmcu=$DEVICE -msmall -I"$MSP430GCC_SUPPORT_PATH/include" \
        -c "$TMP_DIR/ckpt.s" -o "$TMP_DIR/ckpt.o"
    cp "$TMP_DIR/ckpt.o" "${OUTPUT}.o"
    cp "$TMP_DIR/ckpt.s" "${OUTPUT}.s"
fi

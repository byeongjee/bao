#!/usr/bin/env bash
#
# Compile C source with MILP checkpoint insertion.
#
# Usage:
#   compile_milp.sh -e <energy_config> -m <milp_config> [options] <input.c>
#
# Options:
#   -e <config>          Energy estimator config JSON (required)
#   -m <config>          MILP config JSON (required)
#   -o <output>          Output base name (default: build/<input>)
#   -O <level>           LLC optimization level (default: 2)
#   -Oc <level>          Clang optimization level (default: 2)
#   -I <dir>             Add include directory (can be repeated)
#   --local              Compile for host machine instead of MSP430
#   --verbose            Show detailed pass output
#   --debug              Enable DEBUG output
#   --add-debug-markers  Insert mock-counter debug markers
#   -h, --help           Show this help message
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib/common.sh"

# Defaults
ESTIMATOR_CONFIG=""
MILP_CONFIG=""
OUTPUT=""
OPT_LEVEL="2"
CLANG_OPT_LEVEL="2"
EXTRA_INCLUDES=""
LOCAL_MODE="false"
VERBOSE="false"
DEBUG_MODE="false"
ADD_DEBUG_MARKERS="false"
INPUT=""

usage() { sed -n '2,20p' "$0" | sed 's/^# \?//'; exit 0; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        -e|--energy-config) ESTIMATOR_CONFIG="$2"; shift 2 ;;
        -m|--milp-config) MILP_CONFIG="$2"; shift 2 ;;
        -o) OUTPUT="$2"; shift 2 ;;
        -O) OPT_LEVEL="$2"; shift 2 ;;
        -Oc) CLANG_OPT_LEVEL="$2"; shift 2 ;;
        -I) EXTRA_INCLUDES="$EXTRA_INCLUDES -I$2"; shift 2 ;;
        --local) LOCAL_MODE="true"; shift ;;
        --verbose) VERBOSE="true"; shift ;;
        --debug) DEBUG_MODE="true"; shift ;;
        --add-debug-markers) ADD_DEBUG_MARKERS="true"; shift ;;
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
[[ -z "$MILP_CONFIG" ]] && error "MILP config required: use -m <config.json>"
[[ ! -f "$MILP_CONFIG" ]] && error "MILP config not found: $MILP_CONFIG"
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

info "Compiling with MILP..."
compile_to_ir "${IR_ARGS[@]}"

# TripCount annotation (before optimization)
if ! $OPT -load-pass-plugin="$PASS_LIB" \
    -passes=tripcount-annotation \
    -S "$TMP_DIR/input.ll" -o "$TMP_DIR/tripcount.ll"; then
    error "TripCountAnnotation pass failed"
fi

# Frontend optimization (if -Oc > 0)
MILP_INPUT_LL="$TMP_DIR/tripcount.ll"
if [[ "$CLANG_OPT_LEVEL" != "0" ]]; then
    if ! $OPT -passes="default<O$CLANG_OPT_LEVEL>" \
        -vectorize-loops=false -vectorize-slp=false \
        -S "$TMP_DIR/tripcount.ll" -o "$TMP_DIR/input_optimized.ll"; then
        error "IR optimization pipeline failed at -O$CLANG_OPT_LEVEL"
    fi
    MILP_INPUT_LL="$TMP_DIR/input_optimized.ll"
fi

# BB frequency collection: instrument, compile, run, get bb_freq.json
PROFILE_START=$(_now_ms)
info "Collecting BB frequencies..."
if ! $OPT -load-pass-plugin="$PASS_LIB" \
    -passes=bb-freq-collect \
    -energy-config="$ESTIMATOR_CONFIG" \
    -milp-config="$MILP_CONFIG" \
    -S "$MILP_INPUT_LL" -o "$TMP_DIR/freq_inst.ll" 2>&1; then
    error "BB frequency collection pass failed"
fi

if ! $CLANG -O0 $_SYSROOT_FLAGS \
    "$TMP_DIR/freq_inst.ll" "$BB_FREQ_RUNTIME" \
    -o "$TMP_DIR/freq_run" 2>&1; then
    error "BB frequency runtime compilation failed"
fi

BB_FREQ_JSON="$TMP_DIR/bb_freq.json"
(cd "$TMP_DIR" && ./freq_run) || true
if [[ ! -f "$BB_FREQ_JSON" ]]; then
    error "BB frequency collection did not produce bb_freq.json"
fi
PROFILE_END=$(_now_ms)
echo "Profiling time (ms): $((PROFILE_END - PROFILE_START))"

# MILP pass
MILP_EXTRA_FLAGS=""
[[ "$ADD_DEBUG_MARKERS" == "true" ]] && MILP_EXTRA_FLAGS="-add-debug-markers"
[[ "$VERBOSE" == "true" ]] && MILP_EXTRA_FLAGS="$MILP_EXTRA_FLAGS -loop-strip-mining-verbose -abstract-cfg-verbose"

PASS_LOG=$(mktemp "$TMP_DIR/pass_XXXXXX.log")
if $OPT -load-pass-plugin="$PASS_LIB" \
    -passes=milp \
    -energy-config="$ESTIMATOR_CONFIG" \
    -milp-config="$MILP_CONFIG" \
    -bb-freq-file="$BB_FREQ_JSON" \
    $MILP_EXTRA_FLAGS \
    -S "$MILP_INPUT_LL" -o "$TMP_DIR/ckpt.ll" \
    >"$PASS_LOG" 2>&1; then
    if [[ "$VERBOSE" == "true" ]]; then
        cat "$PASS_LOG"
    else
        head -10 "$PASS_LOG"
    fi
else
    cat "$PASS_LOG" >&2
    error "MILP pass failed (see output above)"
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

#!/usr/bin/env bash
#
# Compile C source with RockClimb checkpoint insertion.
#
# Usage:
#   compile_rockclimb.sh -e <energy_config> -c <rockclimb_config> [options] <input.c>
#
# Options:
#   -e <config>          Energy estimator config JSON (required)
#   -c <config>          RockClimb config JSON (required)
#   -o <output>          Output base name (default: build/<input>)
#   -O <level>           LLC optimization level (default: 2)
#   -Oc <level>          Clang optimization level (default: 2)
#   -I <dir>             Add include directory (can be repeated)
#   --local              Compile for host machine instead of MSP430
#   --verbose            Show detailed pass output
#   --debug              Enable DEBUG output
#   --add-debug-markers  Insert mock-counter debug markers
#   --energy-validate    Run energy-validate pass after RockClimb
#   -h, --help           Show this help message
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib/common.sh"

# Defaults
ESTIMATOR_CONFIG=""
ROCKCLIMB_CONFIG=""
OUTPUT=""
OPT_LEVEL="2"
CLANG_OPT_LEVEL="2"
EXTRA_INCLUDES=""
LOCAL_MODE="false"
VERBOSE="false"
DEBUG_MODE="false"
ADD_DEBUG_MARKERS="false"
ENERGY_VALIDATE="false"
INPUT=""

usage() { sed -n '2,20p' "$0" | sed 's/^# \?//'; exit 0; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        -e|--energy-config) ESTIMATOR_CONFIG="$2"; shift 2 ;;
        -c|--rockclimb-config) ROCKCLIMB_CONFIG="$2"; shift 2 ;;
        -o) OUTPUT="$2"; shift 2 ;;
        -O) OPT_LEVEL="$2"; shift 2 ;;
        -Oc) CLANG_OPT_LEVEL="$2"; shift 2 ;;
        -I) EXTRA_INCLUDES="$EXTRA_INCLUDES -I$2"; shift 2 ;;
        --local) LOCAL_MODE="true"; shift ;;
        --verbose) VERBOSE="true"; shift ;;
        --debug) DEBUG_MODE="true"; shift ;;
        --add-debug-markers) ADD_DEBUG_MARKERS="true"; shift ;;
        --energy-validate) ENERGY_VALIDATE="true"; shift ;;
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
[[ -z "$ROCKCLIMB_CONFIG" ]] && error "RockClimb config required: use -c <config.json>"
[[ ! -f "$ROCKCLIMB_CONFIG" ]] && error "RockClimb config not found: $ROCKCLIMB_CONFIG"
[[ ! -f "$PASS_LIB" ]] && error "Pass not found: $PASS_LIB"

# Output name
BUILD_DIR="$PROJECT_DIR/build"
[[ -z "$OUTPUT" ]] && OUTPUT="$BUILD_DIR/$(basename "$INPUT" .c)"
mkdir -p "$(dirname "$OUTPUT")"

TMP_DIR=$(mktemp -d)
trap "rm -rf $TMP_DIR" EXIT

# Build compile_to_ir args
IR_ARGS=("$INPUT" "$TMP_DIR/input.ll" --clang-opt-level "$CLANG_OPT_LEVEL")
[[ "$LOCAL_MODE" == "true" ]] && IR_ARGS+=(--local)
[[ "$DEBUG_MODE" == "true" ]] && IR_ARGS+=(--debug)
# Forward -I flags
for word in $EXTRA_INCLUDES; do
    if [[ "$word" == -I* ]]; then
        IR_ARGS+=(-I "${word#-I}")
    fi
done

info "Compiling with RockClimb..."
compile_to_ir "${IR_ARGS[@]}"

# RockClimb pass
ROCKCLIMB_EXTRA_FLAGS=""
[[ "$ADD_DEBUG_MARKERS" == "true" ]] && ROCKCLIMB_EXTRA_FLAGS="-add-debug-markers"

PASS_LOG=$(mktemp "$TMP_DIR/pass_XXXXXX.log")
if $OPT -load-pass-plugin="$PASS_LIB" \
    -passes=rockclimb \
    -energy-config="$ESTIMATOR_CONFIG" \
    -rockclimb-config="$ROCKCLIMB_CONFIG" \
    $ROCKCLIMB_EXTRA_FLAGS \
    -S "$TMP_DIR/input.ll" -o "$TMP_DIR/ckpt.ll" \
    >"$PASS_LOG" 2>&1; then
    if [[ "$VERBOSE" == "true" ]]; then
        cat "$PASS_LOG"
    else
        grep -E "^(Region|Memory|Inserted|===)" "$PASS_LOG" | head -10
    fi
else
    cat "$PASS_LOG" >&2
    error "RockClimb pass failed (see output above)"
fi

# Energy-validate pass
if [[ "$ENERGY_VALIDATE" == "true" ]]; then
    info "Running energy-validate pass..."
    VALIDATE_FLAGS="-energy-config=$ESTIMATOR_CONFIG -rockclimb-config=$ROCKCLIMB_CONFIG -validate-mode=rockclimb"
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

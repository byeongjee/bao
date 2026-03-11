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
#   --trace <file>       Use pre-collected trace (skip collection)
#   --trace-only         Collect trace and stop; output <output>_trace.json
#   --link               Assemble + link to produce .elf (real runtime: boot.S + runtime.c)
#   --debug-counters     Link debug counter runtime alongside real runtime (implies --link)
#   --halt-mode          Pass -DHALT_MODE when assembling boot.S
#   --linker <script>    Linker script (default: rockclimb_msp430fr5994.ld)
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
TRACE_FILE=""
TRACE_ONLY="false"
LINK="false"
DEBUG_COUNTERS="false"
HALT_MODE="false"
LINKER_SCRIPT=""
INPUT=""

usage() { sed -n '2,26p' "$0" | sed 's/^# \?//'; exit 0; }

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
        --trace) TRACE_FILE="$2"; shift 2 ;;
        --trace-only) TRACE_ONLY="true"; shift ;;
        --link) LINK="true"; shift ;;
        --debug-counters) DEBUG_COUNTERS="true"; LINK="true"; shift ;;
        --halt-mode) HALT_MODE="true"; shift ;;
        --linker) LINKER_SCRIPT="$2"; shift 2 ;;
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

    # Optional: Assemble + link to produce .elf
    if [[ "$LINK" == "true" ]]; then
        info "Linking with SCHEMATIC runtime..."

        # Determine linker script
        [[ -z "$LINKER_SCRIPT" ]] && LINKER_SCRIPT="$SCHEMATIC_LINKER"

        # Assemble boot.S
        BOOT_ASM_FLAGS=""
        [[ "$DEBUG_COUNTERS" == "true" ]] && BOOT_ASM_FLAGS="$BOOT_ASM_FLAGS -DDEBUG_COUNTERS"
        [[ "$HALT_MODE" == "true" ]] && BOOT_ASM_FLAGS="$BOOT_ASM_FLAGS -DHALT_MODE"

        $GCC -mmcu=$DEVICE -msmall $BOOT_ASM_FLAGS \
            -c "$SCHEMATIC_BOOT" -o "$TMP_DIR/boot.o"

        # Compile runtime.c
        $GCC -mmcu=$DEVICE -msmall -O2 \
            -I"$MSP430GCC_SUPPORT_PATH/include" \
            -I"$PROJECT_DIR/passes/runtime" \
            -c "$SCHEMATIC_RUNTIME" -o "$TMP_DIR/runtime.o"

        LINK_OBJS=("$TMP_DIR/ckpt.o" "$TMP_DIR/boot.o" "$TMP_DIR/runtime.o")

        if [[ "$DEBUG_COUNTERS" == "true" ]]; then
            $GCC -mmcu=$DEVICE -msmall -O2 -DDEBUG_COUNTERS \
                -I"$MSP430GCC_SUPPORT_PATH/include" \
                -I"$PROJECT_DIR/passes/runtime" \
                -c "$SCHEMATIC_DEBUG_COUNTERS" -o "$TMP_DIR/debug_counters.o"

            LINK_OBJS+=("$TMP_DIR/debug_counters.o")
        fi

        $GCC -mmcu=$DEVICE -msmall \
            -L"$MSP430GCC_SUPPORT_PATH/include" \
            -T "$LINKER_SCRIPT" \
            -Wl,--nmagic \
            "${LINK_OBJS[@]}" -o "${OUTPUT}.elf"

        $SIZE "${OUTPUT}.elf"
    fi
fi

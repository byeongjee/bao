#!/usr/bin/env bash
#
# Orchestrator: compile with checkpoint insertion, link, and run/flash.
#
# Dispatches to compile_rockclimb.sh, compile_milp.sh, or compile_schematic.sh,
# then links with the appropriate runtime and runs or flashes.
#
# Usage:
#   ./scripts/compile_and_run.sh [options] <input.c>
#
# Options:
#   --mode <mode>    Checkpoint mode: none, rockclimb, milp, schematic (default: none)
#   --runtime <type> Runtime variant: real (default), mock-counter
#   --local          Compile for host machine instead of MSP430 (run locally)
#   --debug          Enable DEBUG output via UART
#   --compile-only   Compile but don't flash
#   --flash-only     Flash existing binary
#   -o <output>      Output base name (default: build/<input>)
#   -e <config>      Energy estimator config JSON (required for all modes)
#   -m <config>      MILP config JSON (required for milp mode)
#   -c <config>      RockClimb config JSON (required for rockclimb mode)
#   -s <config>      SCHEMATIC config JSON (required for schematic mode)
#   -t <trace>       Pre-collected SCHEMATIC trace JSON (skip trace collection)
#   -O <level>       LLC optimization level (default: 2)
#   -Oc <level>      Clang optimization level (default: 2)
#   -I <dir>         Add include directory (can be repeated)
#   --verbose        Show detailed pass output
#   -h, --help       Show this help message
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib/common.sh"

# Defaults
MODE="none"
RUNTIME_TYPE="real"
RUNTIME_SET="false"
LOCAL_MODE="false"
DEBUG_MODE="false"
COMPILE_ONLY="false"
FLASH_ONLY="false"
VERBOSE="false"
OUTPUT=""
OPT_LEVEL=""
CLANG_OPT_LEVEL=""
EXTRA_INCLUDES=""
BUILD_DIR="$PROJECT_DIR/build"

ESTIMATOR_CONFIG=""
MILP_CONFIG=""
ROCKCLIMB_CONFIG=""
SCHEMATIC_CONFIG=""
SCHEMATIC_TRACE=""
usage() { sed -n '2,31p' "$0" | sed 's/^# \?//'; exit 0; }

# Link checkpoint object with mock counter or real runtime.
# Usage: link_runtime <mock_counter> <runtime> <boot> <linker>
link_runtime() {
    local mock_counter="$1" runtime="$2" boot="$3" linker="$4"

    GCC_DEBUG_FLAGS=""
    [[ "$DEBUG_MODE" == "true" ]] && GCC_DEBUG_FLAGS="-DDEBUG"

    case "$RUNTIME_TYPE" in
    mock-counter)
        info "Using mock counter runtime (no power failure recovery)"
        $GCC -mmcu=$DEVICE -O2 -msmall $GCC_DEBUG_FLAGS \
            -I"$MSP430GCC_SUPPORT_PATH/include" \
            -I"$PROJECT_DIR/passes/runtime" \
            -c "$mock_counter" -o "$TMP_DIR/mock.o"

        $GCC -mmcu=$DEVICE -msmall -L"$MSP430GCC_SUPPORT_PATH/include" \
            "$TMP_DIR/ckpt.o" "$TMP_DIR/mock.o" -o "${OUTPUT}.elf"
        ;;
    real)
        info "Using real runtime (with power failure recovery)"
        $GCC -mmcu=$DEVICE -O2 -msmall $GCC_DEBUG_FLAGS \
            -I"$MSP430GCC_SUPPORT_PATH/include" \
            -I"$PROJECT_DIR/passes/runtime" \
            -c "$runtime" -o "$TMP_DIR/runtime.o"

        $GCC -mmcu=$DEVICE -msmall \
            -c "$boot" -o "$TMP_DIR/boot.o"

        $GCC -mmcu=$DEVICE -msmall -L"$MSP430GCC_SUPPORT_PATH/include" \
            -T "$linker" \
            "$TMP_DIR/ckpt.o" "$TMP_DIR/runtime.o" "$TMP_DIR/boot.o" \
            -o "${OUTPUT}.elf"
        ;;
    *)
        error "Unknown runtime type: $RUNTIME_TYPE (use: real, mock-counter)"
        ;;
    esac
}

# Compile LLVM IR to a host-native executable.
# Usage: link_local <ll_file> [mock_counter_source]
link_local() {
    local ll_file="$1"
    local mock_counter="${2:-}"

    if [[ -n "$mock_counter" ]]; then
        $CLANG -O"$OPT_LEVEL" $_SYSROOT_FLAGS \
            -I"$PROJECT_DIR/passes/runtime" \
            "$ll_file" "$mock_counter" -o "${OUTPUT}"
    else
        $CLANG -O"$OPT_LEVEL" $_SYSROOT_FLAGS "$ll_file" -o "${OUTPUT}"
    fi
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode) MODE="$2"; shift 2 ;;
        --runtime) RUNTIME_TYPE="$2"; RUNTIME_SET="true"; shift 2 ;;
        --local) LOCAL_MODE="true"; shift ;;
        --debug) DEBUG_MODE="true"; shift ;;
        --compile-only) COMPILE_ONLY="true"; shift ;;
        --flash-only) FLASH_ONLY="true"; shift ;;
        --verbose) VERBOSE="true"; shift ;;
        -o) OUTPUT="$2"; shift 2 ;;
        -e|--energy-config) ESTIMATOR_CONFIG="$2"; shift 2 ;;
        -m|--milp-config) MILP_CONFIG="$2"; shift 2 ;;
        -c|--rockclimb-config) ROCKCLIMB_CONFIG="$2"; shift 2 ;;
        -s|--schematic-config) SCHEMATIC_CONFIG="$2"; shift 2 ;;
        -t|--schematic-trace) SCHEMATIC_TRACE="$2"; shift 2 ;;
        -O) OPT_LEVEL="$2"; shift 2 ;;
        -Oc) CLANG_OPT_LEVEL="$2"; shift 2 ;;
        -I) EXTRA_INCLUDES="$EXTRA_INCLUDES -I$2"; shift 2 ;;
        -h|--help) usage ;;
        -*) error "Unknown option: $1" ;;
        *) INPUT="$1"; shift ;;
    esac
done

# Validate --local flag combinations
if [[ "$LOCAL_MODE" == "true" ]]; then
    [[ "$FLASH_ONLY" == "true" ]] && \
        error "--local and --flash-only are incompatible"
    if [[ "$MODE" != "none" && "$RUNTIME_SET" != "true" ]]; then
        error "--local requires --runtime (e.g., --runtime mock-counter)"
    fi
    [[ "$RUNTIME_SET" == "true" && "$RUNTIME_TYPE" == "real" ]] && \
        error "--local is incompatible with --runtime real (real runtime needs MSP430 hardware)"
fi

# Validate required configs per mode
if [[ "$FLASH_ONLY" != "true" ]]; then
    case "$MODE" in
        milp)
            [[ -z "$ESTIMATOR_CONFIG" ]] && error "Energy estimator config required for milp mode: use -e <config.json>"
            [[ ! -f "$ESTIMATOR_CONFIG" ]] && error "Estimator config not found: $ESTIMATOR_CONFIG"
            [[ -z "$MILP_CONFIG" ]] && error "MILP config required for milp mode: use -m <config.json>"
            [[ ! -f "$MILP_CONFIG" ]] && error "MILP config not found: $MILP_CONFIG"
            ;;
        rockclimb)
            [[ -z "$ESTIMATOR_CONFIG" ]] && error "Energy estimator config required for rockclimb mode: use -e <config.json>"
            [[ ! -f "$ESTIMATOR_CONFIG" ]] && error "Estimator config not found: $ESTIMATOR_CONFIG"
            [[ -z "$ROCKCLIMB_CONFIG" ]] && error "RockClimb config required: use -c <config.json>"
            [[ ! -f "$ROCKCLIMB_CONFIG" ]] && error "RockClimb config not found: $ROCKCLIMB_CONFIG"
            ;;
        schematic)
            [[ -z "$ESTIMATOR_CONFIG" ]] && error "Energy estimator config required for schematic mode: use -e <config.json>"
            [[ ! -f "$ESTIMATOR_CONFIG" ]] && error "Estimator config not found: $ESTIMATOR_CONFIG"
            [[ -z "$SCHEMATIC_CONFIG" ]] && error "SCHEMATIC config required: use -s <config.json>"
            [[ ! -f "$SCHEMATIC_CONFIG" ]] && error "SCHEMATIC config not found: $SCHEMATIC_CONFIG"
            ;;
        none)
            ;;
    esac
fi

# Validate input
[[ "$FLASH_ONLY" != "true" && -z "$INPUT" ]] && error "No input file specified"
[[ "$FLASH_ONLY" != "true" && ! -f "$INPUT" ]] && error "File not found: $INPUT"

# Output name
[[ -z "$OUTPUT" ]] && OUTPUT="$BUILD_DIR/$(basename "${INPUT:-.}" .c)"
mkdir -p "$(dirname "$OUTPUT")"

# Default optimization levels
[[ -z "$OPT_LEVEL" ]] && OPT_LEVEL="2"
[[ -z "$CLANG_OPT_LEVEL" ]] && CLANG_OPT_LEVEL="2"

TMP_DIR=$(mktemp -d)
trap "rm -rf $TMP_DIR" EXIT

echo "=========================================="
echo "Mode: $MODE | Runtime: $RUNTIME_TYPE | Target: $(if [[ "$LOCAL_MODE" == "true" ]]; then echo "host"; else echo "msp430"; fi) | Debug: $DEBUG_MODE"
echo "Output: $OUTPUT"
echo "=========================================="

if [[ "$FLASH_ONLY" != "true" ]]; then
    # Build common flags for compile scripts
    COMPILE_ARGS=(-e "$ESTIMATOR_CONFIG" -o "$TMP_DIR/ckpt")
    [[ "$LOCAL_MODE" == "true" ]] && COMPILE_ARGS+=(--local)
    [[ "$VERBOSE" == "true" ]] && COMPILE_ARGS+=(--verbose)
    [[ "$DEBUG_MODE" == "true" ]] && COMPILE_ARGS+=(--debug)
    [[ "$RUNTIME_TYPE" == "mock-counter" ]] && COMPILE_ARGS+=(--add-debug-markers)
    [[ -n "$OPT_LEVEL" ]] && COMPILE_ARGS+=(-O "$OPT_LEVEL")
    [[ -n "$CLANG_OPT_LEVEL" ]] && COMPILE_ARGS+=(-Oc "$CLANG_OPT_LEVEL")
    # Forward -I flags
    for word in $EXTRA_INCLUDES; do
        if [[ "$word" == -I* ]]; then
            COMPILE_ARGS+=(-I "${word#-I}")
        fi
    done

    case "$MODE" in
        none)
            info "Compiling without checkpointing (via LLVM)..."

            # Build compile_to_ir args
            IR_ARGS=("$INPUT" "$TMP_DIR/input.ll" --clang-opt-level "$CLANG_OPT_LEVEL")
            [[ "$LOCAL_MODE" == "true" ]] && IR_ARGS+=(--local)
            [[ "$DEBUG_MODE" == "true" ]] && IR_ARGS+=(--debug)
            for word in $EXTRA_INCLUDES; do
                if [[ "$word" == -I* ]]; then
                    IR_ARGS+=(-I "${word#-I}")
                fi
            done
            compile_to_ir "${IR_ARGS[@]}"

            if [[ "$LOCAL_MODE" == "true" ]]; then
                link_local "$TMP_DIR/input.ll"
            else
                $LLC -march=msp430 -O"$OPT_LEVEL" "$TMP_DIR/input.ll" -o "$TMP_DIR/output.s"
                $GCC -mmcu=$DEVICE -msmall -I"$MSP430GCC_SUPPORT_PATH/include" \
                    -L"$MSP430GCC_SUPPORT_PATH/include" \
                    "$TMP_DIR/output.s" -o "${OUTPUT}.elf"
                cp "$TMP_DIR/output.s" "${OUTPUT}.s"
            fi
            ;;

        milp)
            "$SCRIPT_DIR/compile_milp.sh" \
                -m "$MILP_CONFIG" \
                "${COMPILE_ARGS[@]}" \
                "$INPUT"

            if [[ "$LOCAL_MODE" == "true" ]]; then
                link_local "$TMP_DIR/ckpt.ll" "$MILP_MOCK_CKPT_COUNTER"
            else
                link_runtime "$MILP_MOCK_CKPT_COUNTER" "$MILP_RUNTIME" \
                    "$MILP_BOOT" "$MILP_LINKER"
                cp "$TMP_DIR/ckpt.s" "${OUTPUT}.s"
            fi
            ;;

        schematic)
            SCHEMATIC_ARGS=(-s "$SCHEMATIC_CONFIG")
            [[ -n "$SCHEMATIC_TRACE" ]] && SCHEMATIC_ARGS+=(--trace "$SCHEMATIC_TRACE")

            "$SCRIPT_DIR/compile_schematic.sh" \
                "${SCHEMATIC_ARGS[@]}" \
                "${COMPILE_ARGS[@]}" \
                "$INPUT"

            if [[ "$LOCAL_MODE" == "true" ]]; then
                link_local "$TMP_DIR/ckpt.ll" "$SCHEMATIC_MOCK_CKPT_COUNTER"
            else
                link_runtime "$SCHEMATIC_MOCK_CKPT_COUNTER" "$MILP_RUNTIME" \
                    "$MILP_BOOT" "$MILP_LINKER"
                cp "$TMP_DIR/ckpt.s" "${OUTPUT}.s"
            fi
            ;;

        rockclimb)
            MACHINE_ARGS=(-e "$ESTIMATOR_CONFIG" -c "$ROCKCLIMB_CONFIG" -o "$TMP_DIR/ckpt")
            [[ "$VERBOSE" == "true" ]] && MACHINE_ARGS+=(--verbose)
            [[ -n "$CLANG_OPT_LEVEL" ]] && MACHINE_ARGS+=(-Oc "$CLANG_OPT_LEVEL")
            MACHINE_ARGS+=(--link)
            [[ "$RUNTIME_TYPE" == "mock-counter" ]] && MACHINE_ARGS+=(--mock-counter)

            "$SCRIPT_DIR/compile_rockclimb.sh" \
                "${MACHINE_ARGS[@]}" \
                "$INPUT"

            cp "$TMP_DIR/ckpt.elf" "${OUTPUT}.elf" 2>/dev/null || true
            cp "$TMP_DIR/ckpt.s" "${OUTPUT}.s" 2>/dev/null || true
            ;;

        *)
            error "Unknown mode: $MODE (use: none, rockclimb, milp, schematic)"
            ;;
    esac

    if [[ "$LOCAL_MODE" == "true" ]]; then
        size "${OUTPUT}" 2>/dev/null || true
    else
        $SIZE "${OUTPUT}.elf"
    fi
fi

if [[ "$COMPILE_ONLY" != "true" ]]; then
    if [[ "$LOCAL_MODE" == "true" ]]; then
        info "Running locally..."
        EXEC_START=$(_now_ms)
        "${OUTPUT}" || true
        EXEC_END=$(_now_ms)
        echo "Execution time (ms): $((EXEC_END - EXEC_START))"
    else
        info "Flashing..."
        mspdebug tilib "prog ${OUTPUT}.elf"
        echo ""
        [[ "$DEBUG_MODE" == "true" ]] && echo "Serial: screen /dev/tty.usbmodem* 9600"
    fi
fi

echo "Done."

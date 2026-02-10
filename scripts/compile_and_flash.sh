#!/usr/bin/env bash
#
# Compile and Flash Script for MSP430FR5994
#
# Usage:
#   ./scripts/compile_and_flash.sh [options] <input.c>
#
# Options:
#   --mode <mode>    Checkpoint mode: none, rockclimb, milp (default: none)
#   --memory         Enable memory checkpointing (for rockclimb)
#   --runtime <type> Runtime variant: real (default), mock-counter
#   --local          Compile for host machine instead of MSP430 (run locally)
#   --debug          Enable DEBUG output via UART
#   --compile-only   Compile but don't flash
#   --flash-only     Flash existing binary
#   -o <output>      Output base name (default: build/<input>)
#   -c <config>      RockClimb config JSON (default: tests/rockclimb_config.json)
#   -e <config>      MILP energy config JSON (default: tests/simple_config.json)
#   -O <level>       LLC optimization level (default: 2)
#   -Oc <level>      Clang optimization level (default: 2)
#   -I <dir>         Add include directory (can be repeated)
#   --verbose        Show detailed pass output
#   -h, --help       Show this help message
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Load environment
[[ -f "$PROJECT_DIR/.envrc" ]] && source "$PROJECT_DIR/.envrc" 2>/dev/null || true

# MSP430 toolchain
export MSP430GCC_TOOLCHAIN_PATH="${MSP430GCC_TOOLCHAIN_PATH:-$HOME/ti/msp430-gcc}"
export MSP430GCC_SUPPORT_PATH="${MSP430GCC_SUPPORT_PATH:-$HOME/ti/msp430-gcc}"
export PATH="$MSP430GCC_TOOLCHAIN_PATH/bin:$PATH"

# LLVM tools
CLANG="${LLVM_DIR:+$LLVM_DIR/bin/}clang"
OPT="${LLVM_DIR:+$LLVM_DIR/bin/}opt"
LLC="${LLVM_DIR:+$LLVM_DIR/bin/}llc"

# GCC tools
GCC="msp430-elf-gcc"
SIZE="msp430-elf-size"

# Defaults
MODE="none"
MEMORY_CKPT="false"
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
DEVICE="MSP430FR5994"
BUILD_DIR="$PROJECT_DIR/build"

PASS_LIB="$PROJECT_DIR/passes/build/CheckpointPass.so"

ROCKCLIMB_CONFIG="$PROJECT_DIR/tests/rockclimb_config.json"
ROCKCLIMB_RUNTIME="$PROJECT_DIR/passes/runtime/rockclimb_runtime.c"
ROCKCLIMB_BOOT="$PROJECT_DIR/passes/runtime/rockclimb_boot.S"
ROCKCLIMB_MOCK_CKPT_COUNTER="$PROJECT_DIR/passes/runtime/rockclimb_mock_ckpt_counter.c"
ROCKCLIMB_LINKER="$PROJECT_DIR/passes/runtime/rockclimb_msp430fr5994.ld"

MILP_ENERGY_CONFIG="$PROJECT_DIR/tests/simple_config.json"
MILP_RUNTIME="$PROJECT_DIR/passes/runtime/milp_runtime.c"
MILP_BOOT="$PROJECT_DIR/passes/runtime/milp_boot.S"
MILP_MOCK_CKPT_COUNTER="$PROJECT_DIR/passes/runtime/milp_mock_ckpt_counter.c"
MILP_LINKER="$PROJECT_DIR/passes/runtime/milp_msp430fr5994.ld"

usage() { sed -n '2,24p' "$0" | sed 's/^# \?//'; exit 0; }
error() { echo -e "\033[0;31mError: $1\033[0m" >&2; exit 1; }
info() { echo -e "\033[0;36m$1\033[0m"; }

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
    local sysroot_flags=""

    # Source-built clang may not know the SDK path on macOS
    if command -v xcrun &>/dev/null; then
        sysroot_flags="-isysroot $(xcrun --show-sdk-path)"
    fi

    if [[ -n "$mock_counter" ]]; then
        $CLANG -O"$OPT_LEVEL" $sysroot_flags \
            -I"$PROJECT_DIR/passes/runtime" \
            "$ll_file" "$mock_counter" -o "${OUTPUT}"
    else
        $CLANG -O"$OPT_LEVEL" $sysroot_flags "$ll_file" -o "${OUTPUT}"
    fi
}

# Compile C source to LLVM IR.
compile_to_ir() {
    if [[ "$LOCAL_MODE" == "true" ]]; then
        CLANG_FLAGS="-S -emit-llvm -O$CLANG_OPT_LEVEL"
        CLANG_FLAGS="$CLANG_FLAGS -I$PROJECT_DIR/passes/include"
    else
        CLANG_FLAGS="--target=msp430-elf -S -emit-llvm -O$CLANG_OPT_LEVEL -D__MSP430FR5994__"
        CLANG_FLAGS="$CLANG_FLAGS -I$PROJECT_DIR/passes/include -I$MSP430GCC_SUPPORT_PATH/include -I$MSP430GCC_SUPPORT_PATH/msp430-elf/include"
    fi
    [[ "$CLANG_OPT_LEVEL" == "0" ]] && \
        CLANG_FLAGS="$CLANG_FLAGS -Xclang -disable-O0-optnone"
    CLANG_FLAGS="$CLANG_FLAGS $EXTRA_INCLUDES"
    [[ "$DEBUG_MODE" == "true" ]] && CLANG_FLAGS="$CLANG_FLAGS -DDEBUG"
    $CLANG $CLANG_FLAGS "$INPUT" -o "$TMP_DIR/input.ll"
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode) MODE="$2"; shift 2 ;;
        --memory) MEMORY_CKPT="true"; shift ;;
        --runtime) RUNTIME_TYPE="$2"; RUNTIME_SET="true"; shift 2 ;;
        --local) LOCAL_MODE="true"; shift ;;
        --debug) DEBUG_MODE="true"; shift ;;
        --compile-only) COMPILE_ONLY="true"; shift ;;
        --flash-only) FLASH_ONLY="true"; shift ;;
        --verbose) VERBOSE="true"; shift ;;
        -o) OUTPUT="$2"; shift 2 ;;
        -c) ROCKCLIMB_CONFIG="$2"; shift 2 ;;
        -e|--energy-config) MILP_ENERGY_CONFIG="$2"; shift 2 ;;
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

# Validate
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
    case "$MODE" in
        none)
            info "Compiling without checkpointing (via LLVM)..."

            compile_to_ir

            if [[ "$LOCAL_MODE" == "true" ]]; then
                link_local "$TMP_DIR/input.ll"
            else
                # LLVM IR to assembly
                $LLC -march=msp430 -O"$OPT_LEVEL" "$TMP_DIR/input.ll" -o "$TMP_DIR/output.s"

                # Assemble and link with GCC
                $GCC -mmcu=$DEVICE -msmall -I"$MSP430GCC_SUPPORT_PATH/include" \
                    -L"$MSP430GCC_SUPPORT_PATH/include" \
                    "$TMP_DIR/output.s" -o "${OUTPUT}.elf"

                cp "$TMP_DIR/output.s" "${OUTPUT}.s"
            fi
            ;;

        rockclimb)
            info "Compiling with RockClimb..."
            [[ ! -f "$PASS_LIB" ]] && error "Pass not found: $PASS_LIB"
            [[ ! -f "$ROCKCLIMB_CONFIG" ]] && error "Config not found: $ROCKCLIMB_CONFIG"

            compile_to_ir

            # RockClimb pass
            PASS_OUTPUT=$($OPT -load-pass-plugin="$PASS_LIB" \
                -passes=rockclimb \
                -rockclimb-config="$ROCKCLIMB_CONFIG" \
                -rockclimb-memory-ckpt="$MEMORY_CKPT" \
                -S "$TMP_DIR/input.ll" -o "$TMP_DIR/ckpt.ll" 2>&1)

            if [[ "$VERBOSE" == "true" ]]; then
                echo "$PASS_OUTPUT"
            else
                echo "$PASS_OUTPUT" | grep -E "^(Region|Memory|Inserted|===)" | head -10
            fi

            if [[ "$LOCAL_MODE" == "true" ]]; then
                link_local "$TMP_DIR/ckpt.ll" "$ROCKCLIMB_MOCK_CKPT_COUNTER"
            else
                # LLVM IR to assembly
                $LLC -march=msp430 -O"$OPT_LEVEL" "$TMP_DIR/ckpt.ll" -o "$TMP_DIR/ckpt.s"

                # Assemble with GCC
                $GCC -mmcu=$DEVICE -msmall -I"$MSP430GCC_SUPPORT_PATH/include" \
                    -c "$TMP_DIR/ckpt.s" -o "$TMP_DIR/ckpt.o"

                link_runtime "$ROCKCLIMB_MOCK_CKPT_COUNTER" "$ROCKCLIMB_RUNTIME" \
                    "$ROCKCLIMB_BOOT" "$ROCKCLIMB_LINKER"

                cp "$TMP_DIR/ckpt.s" "${OUTPUT}.s"
            fi
            ;;

        milp)
            info "Compiling with MILP..."
            [[ ! -f "$PASS_LIB" ]] && error "Pass not found: $PASS_LIB"
            [[ ! -f "$MILP_ENERGY_CONFIG" ]] && error "Energy config not found: $MILP_ENERGY_CONFIG"

            compile_to_ir

            # MILP pass
            PASS_OUTPUT=$($OPT -load-pass-plugin="$PASS_LIB" \
                -passes=milp \
                -energy-config="$MILP_ENERGY_CONFIG" \
                -S "$TMP_DIR/input.ll" -o "$TMP_DIR/ckpt.ll" 2>&1)

            if [[ "$VERBOSE" == "true" ]]; then
                echo "$PASS_OUTPUT"
            else
                echo "$PASS_OUTPUT" | head -10
            fi

            if [[ "$LOCAL_MODE" == "true" ]]; then
                link_local "$TMP_DIR/ckpt.ll" "$MILP_MOCK_CKPT_COUNTER"
            else
                # LLVM IR to assembly
                $LLC -march=msp430 -O"$OPT_LEVEL" "$TMP_DIR/ckpt.ll" -o "$TMP_DIR/ckpt.s"

                # Assemble with GCC
                $GCC -mmcu=$DEVICE -msmall -I"$MSP430GCC_SUPPORT_PATH/include" \
                    -c "$TMP_DIR/ckpt.s" -o "$TMP_DIR/ckpt.o"

                link_runtime "$MILP_MOCK_CKPT_COUNTER" "$MILP_RUNTIME" \
                    "$MILP_BOOT" "$MILP_LINKER"

                cp "$TMP_DIR/ckpt.s" "${OUTPUT}.s"
            fi
            ;;

        *)
            error "Unknown mode: $MODE (use: none, rockclimb, milp)"
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
        "${OUTPUT}"
    else
        info "Flashing..."
        mspdebug tilib "prog ${OUTPUT}.elf"
        echo ""
        [[ "$DEBUG_MODE" == "true" ]] && echo "Serial: screen /dev/tty.usbmodem* 9600"
    fi
fi

echo "Done."

#!/usr/bin/env bash
#
# Compile and Flash Script for MSP430FR5994
#
# Usage:
#   ./scripts/compile_and_flash.sh [options] <input.c>
#
# Options:
#   --mode <mode>    Checkpoint mode: none, rockclimb (default: none)
#   --memory         Enable memory checkpointing (for rockclimb)
#   --debug          Enable DEBUG output via UART
#   --compile-only   Compile but don't flash
#   --flash-only     Flash existing binary
#   -o <output>      Output base name (default: input filename)
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
DEBUG_MODE="false"
COMPILE_ONLY="false"
FLASH_ONLY="false"
OUTPUT=""
DEVICE="MSP430FR5994"
BUILD_DIR="$PROJECT_DIR/build"

ROCKCLIMB_PASS="$PROJECT_DIR/passes/build/CheckpointPass.so"
ROCKCLIMB_CONFIG="$PROJECT_DIR/tests/rockclimb_config.json"
ROCKCLIMB_STUBS="$PROJECT_DIR/passes/runtime/rockclimb_stubs.c"

usage() { sed -n '2,16p' "$0" | sed 's/^# \?//'; exit 0; }
error() { echo -e "\033[0;31mError: $1\033[0m" >&2; exit 1; }
info() { echo -e "\033[0;36m$1\033[0m"; }

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode) MODE="$2"; shift 2 ;;
        --memory) MEMORY_CKPT="true"; shift ;;
        --debug) DEBUG_MODE="true"; shift ;;
        --compile-only) COMPILE_ONLY="true"; shift ;;
        --flash-only) FLASH_ONLY="true"; shift ;;
        -o) OUTPUT="$2"; shift 2 ;;
        -h|--help) usage ;;
        -*) error "Unknown option: $1" ;;
        *) INPUT="$1"; shift ;;
    esac
done

# Validate
[[ "$FLASH_ONLY" != "true" && -z "$INPUT" ]] && error "No input file specified"
[[ "$FLASH_ONLY" != "true" && ! -f "$INPUT" ]] && error "File not found: $INPUT"

# Output name
[[ -z "$OUTPUT" ]] && OUTPUT="$BUILD_DIR/$(basename "${INPUT:-.}" .c)"
mkdir -p "$BUILD_DIR"

TMP_DIR=$(mktemp -d)
trap "rm -rf $TMP_DIR" EXIT

echo "=========================================="
echo "Mode: $MODE | Debug: $DEBUG_MODE | Output: $OUTPUT"
echo "=========================================="

if [[ "$FLASH_ONLY" != "true" ]]; then
    case "$MODE" in
        none)
            info "Compiling without checkpointing (via LLVM)..."

            # C to LLVM IR
            CLANG_FLAGS="--target=msp430-elf -S -emit-llvm -O2 -D__MSP430FR5994__"
            CLANG_FLAGS="$CLANG_FLAGS -I$PROJECT_DIR/passes/include -I$MSP430GCC_SUPPORT_PATH/include -I$MSP430GCC_SUPPORT_PATH/msp430-elf/include"
            [[ "$DEBUG_MODE" == "true" ]] && CLANG_FLAGS="$CLANG_FLAGS -DDEBUG"
            $CLANG $CLANG_FLAGS "$INPUT" -o "$TMP_DIR/input.ll"

            # LLVM IR to assembly
            $LLC -march=msp430 -O2 "$TMP_DIR/input.ll" -o "$TMP_DIR/output.s"

            # Assemble and link with GCC
            $GCC -mmcu=$DEVICE -msmall -I"$MSP430GCC_SUPPORT_PATH/include" \
                -L"$MSP430GCC_SUPPORT_PATH/include" \
                "$TMP_DIR/output.s" -o "${OUTPUT}.elf"

            cp "$TMP_DIR/output.s" "${OUTPUT}.s"
            ;;

        rockclimb)
            info "Compiling with RockClimb..."
            [[ ! -f "$ROCKCLIMB_PASS" ]] && error "Pass not found: $ROCKCLIMB_PASS"

            # C to LLVM IR
            CLANG_FLAGS="--target=msp430-elf -S -emit-llvm -O0 -Xclang -disable-O0-optnone -D__MSP430FR5994__"
            CLANG_FLAGS="$CLANG_FLAGS -I$PROJECT_DIR/passes/include -I$MSP430GCC_SUPPORT_PATH/include -I$MSP430GCC_SUPPORT_PATH/msp430-elf/include"
            [[ "$DEBUG_MODE" == "true" ]] && CLANG_FLAGS="$CLANG_FLAGS -DDEBUG"
            $CLANG $CLANG_FLAGS "$INPUT" -o "$TMP_DIR/input.ll"

            # RockClimb pass
            $OPT -load-pass-plugin="$ROCKCLIMB_PASS" \
                -passes=rockclimb \
                -rockclimb-config="$ROCKCLIMB_CONFIG" \
                -rockclimb-memory-ckpt="$MEMORY_CKPT" \
                -S "$TMP_DIR/input.ll" -o "$TMP_DIR/ckpt.ll" 2>&1 | \
                grep -E "^(Region|Memory|Inserted|===)" | head -10

            # LLVM IR to assembly
            $LLC -march=msp430 -O0 "$TMP_DIR/ckpt.ll" -o "$TMP_DIR/ckpt.s"

            # Assemble with GCC
            $GCC -mmcu=$DEVICE -msmall -I"$MSP430GCC_SUPPORT_PATH/include" \
                -c "$TMP_DIR/ckpt.s" -o "$TMP_DIR/ckpt.o"

            # Compile stubs
            $GCC -mmcu=$DEVICE -O2 -msmall -I"$MSP430GCC_SUPPORT_PATH/include" \
                -c "$ROCKCLIMB_STUBS" -o "$TMP_DIR/stubs.o"

            # Link
            $GCC -mmcu=$DEVICE -msmall -L"$MSP430GCC_SUPPORT_PATH/include" \
                "$TMP_DIR/ckpt.o" "$TMP_DIR/stubs.o" -o "${OUTPUT}.elf"

            cp "$TMP_DIR/ckpt.s" "${OUTPUT}.s"
            ;;

        *)
            error "Unknown mode: $MODE"
            ;;
    esac

    $SIZE "${OUTPUT}.elf"
fi

if [[ "$COMPILE_ONLY" != "true" ]]; then
    info "Flashing..."
    mspdebug tilib "prog ${OUTPUT}.elf"
    echo ""
    [[ "$DEBUG_MODE" == "true" ]] && echo "Serial: screen /dev/tty.usbmodem* 9600"
fi

echo "Done."

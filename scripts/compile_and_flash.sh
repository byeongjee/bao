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
#   -o <output>      Output base name (default: build/<input>)
#   -c <config>      RockClimb config JSON (default: tests/rockclimb_config.json)
#   -O <level>       LLC optimization level (default: 2)
#   -Oc <level>      Clang optimization level (default: 2)
#   -I <dir>         Add include directory (can be repeated)
#   --analyze        Show NVM symbols and section analysis
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
OBJDUMP="msp430-elf-objdump"

# Defaults
MODE="none"
MEMORY_CKPT="false"
DEBUG_MODE="false"
COMPILE_ONLY="false"
FLASH_ONLY="false"
VERBOSE="false"
ANALYZE="false"
OUTPUT=""
OPT_LEVEL=""
CLANG_OPT_LEVEL=""
EXTRA_INCLUDES=""
DEVICE="MSP430FR5994"
BUILD_DIR="$PROJECT_DIR/build"

ROCKCLIMB_PASS="$PROJECT_DIR/passes/build/CheckpointPass.so"
ROCKCLIMB_CONFIG="$PROJECT_DIR/tests/rockclimb_config.json"
ROCKCLIMB_STUBS="$PROJECT_DIR/passes/runtime/rockclimb_stubs.c"

usage() { sed -n '2,23p' "$0" | sed 's/^# \?//'; exit 0; }
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
        --verbose) VERBOSE="true"; shift ;;
        --analyze) ANALYZE="true"; shift ;;
        -o) OUTPUT="$2"; shift 2 ;;
        -c) ROCKCLIMB_CONFIG="$2"; shift 2 ;;
        -O) OPT_LEVEL="$2"; shift 2 ;;
        -Oc) CLANG_OPT_LEVEL="$2"; shift 2 ;;
        -I) EXTRA_INCLUDES="$EXTRA_INCLUDES -I$2"; shift 2 ;;
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
mkdir -p "$(dirname "$OUTPUT")"

# Default optimization levels
[[ -z "$OPT_LEVEL" ]] && OPT_LEVEL="2"
[[ -z "$CLANG_OPT_LEVEL" ]] && CLANG_OPT_LEVEL="2"

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
            CLANG_FLAGS="--target=msp430-elf -S -emit-llvm -O$CLANG_OPT_LEVEL -D__MSP430FR5994__"
            CLANG_FLAGS="$CLANG_FLAGS -I$PROJECT_DIR/passes/include -I$MSP430GCC_SUPPORT_PATH/include -I$MSP430GCC_SUPPORT_PATH/msp430-elf/include"
            CLANG_FLAGS="$CLANG_FLAGS $EXTRA_INCLUDES"
            [[ "$DEBUG_MODE" == "true" ]] && CLANG_FLAGS="$CLANG_FLAGS -DDEBUG"
            $CLANG $CLANG_FLAGS "$INPUT" -o "$TMP_DIR/input.ll"

            # LLVM IR to assembly
            $LLC -march=msp430 -O"$OPT_LEVEL" "$TMP_DIR/input.ll" -o "$TMP_DIR/output.s"

            # Assemble and link with GCC
            $GCC -mmcu=$DEVICE -msmall -I"$MSP430GCC_SUPPORT_PATH/include" \
                -L"$MSP430GCC_SUPPORT_PATH/include" \
                "$TMP_DIR/output.s" -o "${OUTPUT}.elf"

            cp "$TMP_DIR/output.s" "${OUTPUT}.s"
            ;;

        rockclimb)
            info "Compiling with RockClimb..."
            [[ ! -f "$ROCKCLIMB_PASS" ]] && error "Pass not found: $ROCKCLIMB_PASS"
            [[ ! -f "$ROCKCLIMB_CONFIG" ]] && error "Config not found: $ROCKCLIMB_CONFIG"

            # C to LLVM IR
            CLANG_FLAGS="--target=msp430-elf -S -emit-llvm -O$CLANG_OPT_LEVEL -D__MSP430FR5994__"
            [[ "$CLANG_OPT_LEVEL" == "0" ]] && CLANG_FLAGS="$CLANG_FLAGS -Xclang -disable-O0-optnone"
            CLANG_FLAGS="$CLANG_FLAGS -I$PROJECT_DIR/passes/include -I$MSP430GCC_SUPPORT_PATH/include -I$MSP430GCC_SUPPORT_PATH/msp430-elf/include"
            CLANG_FLAGS="$CLANG_FLAGS $EXTRA_INCLUDES"
            [[ "$DEBUG_MODE" == "true" ]] && CLANG_FLAGS="$CLANG_FLAGS -DDEBUG"
            $CLANG $CLANG_FLAGS "$INPUT" -o "$TMP_DIR/input.ll"

            # RockClimb pass
            PASS_OUTPUT=$($OPT -load-pass-plugin="$ROCKCLIMB_PASS" \
                -passes=rockclimb \
                -rockclimb-config="$ROCKCLIMB_CONFIG" \
                -rockclimb-memory-ckpt="$MEMORY_CKPT" \
                -S "$TMP_DIR/input.ll" -o "$TMP_DIR/ckpt.ll" 2>&1)

            if [[ "$VERBOSE" == "true" ]]; then
                echo "$PASS_OUTPUT"
            else
                echo "$PASS_OUTPUT" | grep -E "^(Region|Memory|Inserted|===)" | head -10
            fi

            # LLVM IR to assembly
            $LLC -march=msp430 -O"$OPT_LEVEL" "$TMP_DIR/ckpt.ll" -o "$TMP_DIR/ckpt.s"

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

            # Analysis
            if [[ "$ANALYZE" == "true" ]]; then
                echo ""
                info "=== Analysis ==="
                echo "NVM Symbols:"
                $OBJDUMP -t "$TMP_DIR/ckpt.o" 2>/dev/null | grep -E "__nvm" | while read line; do
                    echo "  $line"
                done
                echo ""
                echo "External Dependencies:"
                $OBJDUMP -t "$TMP_DIR/ckpt.o" 2>/dev/null | grep "\*UND\*" | awk '{print "  " $NF}'
            fi
            ;;

        *)
            error "Unknown mode: $MODE (use: none, rockclimb)"
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

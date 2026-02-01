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
#   --stubs          Use stub runtime for testing under constant power
#                    (Default: uses real runtime with power failure recovery)
#   --debug          Enable DEBUG output via UART
#   --compile-only   Compile but don't flash
#   --flash-only     Flash existing binary
#   -o <output>      Output base name (default: build/<input>)
#   -c <config>      RockClimb config JSON (default: tests/rockclimb_config.json)
#   -e <config>      MILP energy config JSON (default: tests/simple_config.json)
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
USE_STUBS="false"
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

PASS_LIB="$PROJECT_DIR/passes/build/CheckpointPass.so"

ROCKCLIMB_CONFIG="$PROJECT_DIR/tests/rockclimb_config.json"
ROCKCLIMB_RUNTIME="$PROJECT_DIR/passes/runtime/rockclimb_runtime.c"
ROCKCLIMB_BOOT="$PROJECT_DIR/passes/runtime/rockclimb_boot.S"
ROCKCLIMB_STUBS="$PROJECT_DIR/passes/runtime/rockclimb_stubs.c"
ROCKCLIMB_LINKER="$PROJECT_DIR/passes/runtime/rockclimb_msp430fr5994.ld"

MILP_ENERGY_CONFIG="$PROJECT_DIR/tests/simple_config.json"
MILP_RUNTIME="$PROJECT_DIR/passes/runtime/milp_runtime.c"
MILP_BOOT="$PROJECT_DIR/passes/runtime/milp_boot.S"
MILP_STUBS="$PROJECT_DIR/passes/runtime/milp_stubs.c"
MILP_LINKER="$PROJECT_DIR/passes/runtime/milp_msp430fr5994.ld"

usage() { sed -n '2,25p' "$0" | sed 's/^# \?//'; exit 0; }
error() { echo -e "\033[0;31mError: $1\033[0m" >&2; exit 1; }
info() { echo -e "\033[0;36m$1\033[0m"; }

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode) MODE="$2"; shift 2 ;;
        --memory) MEMORY_CKPT="true"; shift ;;
        --stubs) USE_STUBS="true"; shift ;;
        --debug) DEBUG_MODE="true"; shift ;;
        --compile-only) COMPILE_ONLY="true"; shift ;;
        --flash-only) FLASH_ONLY="true"; shift ;;
        --verbose) VERBOSE="true"; shift ;;
        --analyze) ANALYZE="true"; shift ;;
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
echo "Mode: $MODE | Stubs: $USE_STUBS | Debug: $DEBUG_MODE"
echo "Output: $OUTPUT"
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
            [[ ! -f "$PASS_LIB" ]] && error "Pass not found: $PASS_LIB"
            [[ ! -f "$ROCKCLIMB_CONFIG" ]] && error "Config not found: $ROCKCLIMB_CONFIG"

            # C to LLVM IR
            CLANG_FLAGS="--target=msp430-elf -S -emit-llvm -O$CLANG_OPT_LEVEL -D__MSP430FR5994__"
            [[ "$CLANG_OPT_LEVEL" == "0" ]] && CLANG_FLAGS="$CLANG_FLAGS -Xclang -disable-O0-optnone"
            CLANG_FLAGS="$CLANG_FLAGS -I$PROJECT_DIR/passes/include -I$MSP430GCC_SUPPORT_PATH/include -I$MSP430GCC_SUPPORT_PATH/msp430-elf/include"
            CLANG_FLAGS="$CLANG_FLAGS $EXTRA_INCLUDES"
            [[ "$DEBUG_MODE" == "true" ]] && CLANG_FLAGS="$CLANG_FLAGS -DDEBUG"
            $CLANG $CLANG_FLAGS "$INPUT" -o "$TMP_DIR/input.ll"

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

            # LLVM IR to assembly
            $LLC -march=msp430 -O"$OPT_LEVEL" "$TMP_DIR/ckpt.ll" -o "$TMP_DIR/ckpt.s"

            # Assemble with GCC
            $GCC -mmcu=$DEVICE -msmall -I"$MSP430GCC_SUPPORT_PATH/include" \
                -c "$TMP_DIR/ckpt.s" -o "$TMP_DIR/ckpt.o"

            if [[ "$USE_STUBS" == "true" ]]; then
                # Debug mode: use stubs (no real recovery)
                info "Using stub runtime (no power failure recovery)"
                GCC_DEBUG_FLAGS=""
                [[ "$DEBUG_MODE" == "true" ]] && GCC_DEBUG_FLAGS="-DDEBUG"
                $GCC -mmcu=$DEVICE -O2 -msmall $GCC_DEBUG_FLAGS -I"$MSP430GCC_SUPPORT_PATH/include" \
                    -c "$ROCKCLIMB_STUBS" -o "$TMP_DIR/stubs.o"

                $GCC -mmcu=$DEVICE -msmall -L"$MSP430GCC_SUPPORT_PATH/include" \
                    "$TMP_DIR/ckpt.o" "$TMP_DIR/stubs.o" -o "${OUTPUT}.elf"
            else
                # Default: real runtime with recovery
                info "Using real runtime (with power failure recovery)"
                $GCC -mmcu=$DEVICE -O2 -msmall \
                    -I"$MSP430GCC_SUPPORT_PATH/include" \
                    -I"$PROJECT_DIR/passes/runtime" \
                    -c "$ROCKCLIMB_RUNTIME" -o "$TMP_DIR/runtime.o"

                $GCC -mmcu=$DEVICE -msmall \
                    -c "$ROCKCLIMB_BOOT" -o "$TMP_DIR/boot.o"

                $GCC -mmcu=$DEVICE -msmall -L"$MSP430GCC_SUPPORT_PATH/include" \
                    -T "$ROCKCLIMB_LINKER" \
                    "$TMP_DIR/ckpt.o" "$TMP_DIR/runtime.o" "$TMP_DIR/boot.o" \
                    -o "${OUTPUT}.elf"
            fi

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

        milp)
            info "Compiling with MILP..."
            [[ ! -f "$PASS_LIB" ]] && error "Pass not found: $PASS_LIB"
            [[ ! -f "$MILP_ENERGY_CONFIG" ]] && error "Energy config not found: $MILP_ENERGY_CONFIG"

            # C to LLVM IR
            CLANG_FLAGS="--target=msp430-elf -S -emit-llvm -O$CLANG_OPT_LEVEL -D__MSP430FR5994__"
            [[ "$CLANG_OPT_LEVEL" == "0" ]] && CLANG_FLAGS="$CLANG_FLAGS -Xclang -disable-O0-optnone"
            CLANG_FLAGS="$CLANG_FLAGS -I$PROJECT_DIR/passes/include -I$MSP430GCC_SUPPORT_PATH/include -I$MSP430GCC_SUPPORT_PATH/msp430-elf/include"
            CLANG_FLAGS="$CLANG_FLAGS $EXTRA_INCLUDES"
            [[ "$DEBUG_MODE" == "true" ]] && CLANG_FLAGS="$CLANG_FLAGS -DDEBUG"
            $CLANG $CLANG_FLAGS "$INPUT" -o "$TMP_DIR/input.ll"

            # MILP pass
            PASS_OUTPUT=$($OPT -load-pass-plugin="$PASS_LIB" \
                -passes=milp \
                -energy-config="$MILP_ENERGY_CONFIG" \
                -checkpoint-function=__milp_checkpoint \
                -S "$TMP_DIR/input.ll" -o "$TMP_DIR/ckpt.ll" 2>&1)

            if [[ "$VERBOSE" == "true" ]]; then
                echo "$PASS_OUTPUT"
            else
                echo "$PASS_OUTPUT" | head -10
            fi

            # LLVM IR to assembly
            $LLC -march=msp430 -O"$OPT_LEVEL" "$TMP_DIR/ckpt.ll" -o "$TMP_DIR/ckpt.s"

            # Assemble with GCC
            $GCC -mmcu=$DEVICE -msmall -I"$MSP430GCC_SUPPORT_PATH/include" \
                -c "$TMP_DIR/ckpt.s" -o "$TMP_DIR/ckpt.o"

            GCC_DEBUG_FLAGS=""
            [[ "$DEBUG_MODE" == "true" ]] && GCC_DEBUG_FLAGS="-DDEBUG"

            if [[ "$USE_STUBS" == "true" ]]; then
                info "Using stub runtime (no power failure recovery)"
                $GCC -mmcu=$DEVICE -O2 -msmall $GCC_DEBUG_FLAGS \
                    -I"$MSP430GCC_SUPPORT_PATH/include" \
                    -I"$PROJECT_DIR/passes/runtime" \
                    -c "$MILP_STUBS" -o "$TMP_DIR/stubs.o"

                $GCC -mmcu=$DEVICE -msmall -L"$MSP430GCC_SUPPORT_PATH/include" \
                    "$TMP_DIR/ckpt.o" "$TMP_DIR/stubs.o" -o "${OUTPUT}.elf"
            else
                info "Using real runtime (with power failure recovery stubbed)"
                $GCC -mmcu=$DEVICE -O2 -msmall $GCC_DEBUG_FLAGS \
                    -I"$MSP430GCC_SUPPORT_PATH/include" \
                    -I"$PROJECT_DIR/passes/runtime" \
                    -c "$MILP_RUNTIME" -o "$TMP_DIR/runtime.o"

                $GCC -mmcu=$DEVICE -msmall \
                    -c "$MILP_BOOT" -o "$TMP_DIR/boot.o"

                $GCC -mmcu=$DEVICE -msmall -L"$MSP430GCC_SUPPORT_PATH/include" \
                    -T "$MILP_LINKER" \
                    "$TMP_DIR/ckpt.o" "$TMP_DIR/runtime.o" "$TMP_DIR/boot.o" \
                    -o "${OUTPUT}.elf"
            fi

            cp "$TMP_DIR/ckpt.s" "${OUTPUT}.s"
            ;;

        *)
            error "Unknown mode: $MODE (use: none, rockclimb, milp)"
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

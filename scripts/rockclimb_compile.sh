#!/bin/bash
#
# RockClimb Compile Script
# Compiles C code to checkpointed MSP430 binary using the RockClimb pass.
#
# Usage: ./rockclimb_compile.sh [options] <input.c>
#
# Options:
#   -o <output>      Output base name (default: input filename without .c)
#   -c <config>      RockClimb config JSON (default: tests/rockclimb_config.json)
#   -m, --memory     Enable memory checkpointing (default: disabled)
#   -d, --debug      Show detailed pass output
#   -s, --asm-only   Output assembly only (no object file)
#   -O <level>       Optimization level for LLC (default: 0)
#   -I <dir>         Add include directory (can be used multiple times)
#   --analyze        Show analysis of generated output
#   -h, --help       Show this help message
#
# Environment variables:
#   LLVM_DIR         Path to LLVM build directory
#   ROCKCLIMB_DIR    Path to checkpoint-insertion project (auto-detected)
#

set -e

# Auto-detect project directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROCKCLIMB_DIR="${ROCKCLIMB_DIR:-$(dirname "$SCRIPT_DIR")}"

# Load environment from .envrc or .env if available
if [[ -f "$ROCKCLIMB_DIR/.envrc" ]]; then
    source "$ROCKCLIMB_DIR/.envrc" 2>/dev/null || true
fi
if [[ -f "$ROCKCLIMB_DIR/.env" ]]; then
    source "$ROCKCLIMB_DIR/.env" 2>/dev/null || true
fi

# LLVM tools - use LLVM_DIR if set, otherwise use system tools
if [[ -n "$LLVM_DIR" ]]; then
    CLANG="${CLANG:-$LLVM_DIR/bin/clang}"
    OPT="${OPT:-$LLVM_DIR/bin/opt}"
    LLC="${LLC:-$LLVM_DIR/bin/llc}"
    OBJDUMP="${OBJDUMP:-$LLVM_DIR/bin/llvm-objdump}"
else
    CLANG="${CLANG:-clang}"
    OPT="${OPT:-opt}"
    LLC="${LLC:-llc}"
    OBJDUMP="${OBJDUMP:-llvm-objdump}"
fi

# Defaults
PASS_LIB="$ROCKCLIMB_DIR/passes/build/CheckpointPass.so"
CONFIG="$ROCKCLIMB_DIR/tests/rockclimb_config.json"
MEMORY_CKPT="false"
DEBUG="false"
ASM_ONLY="false"
OPT_LEVEL="0"
ANALYZE="false"
OUTPUT=""
EXTRA_INCLUDES=""

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

usage() {
    sed -n '2,18p' "$0" | sed 's/^# \?//'
    exit 0
}

error() {
    echo -e "${RED}Error: $1${NC}" >&2
    exit 1
}

info() {
    echo -e "${CYAN}$1${NC}"
}

success() {
    echo -e "${GREEN}$1${NC}"
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        -o)
            OUTPUT="$2"
            shift 2
            ;;
        -c)
            CONFIG="$2"
            shift 2
            ;;
        -m|--memory)
            MEMORY_CKPT="true"
            shift
            ;;
        -d|--debug)
            DEBUG="true"
            shift
            ;;
        -s|--asm-only)
            ASM_ONLY="true"
            shift
            ;;
        -O)
            OPT_LEVEL="$2"
            shift 2
            ;;
        --analyze)
            ANALYZE="true"
            shift
            ;;
        -I)
            EXTRA_INCLUDES="$EXTRA_INCLUDES -I$2"
            shift 2
            ;;
        -h|--help)
            usage
            ;;
        -*)
            error "Unknown option: $1"
            ;;
        *)
            INPUT="$1"
            shift
            ;;
    esac
done

# Validate input
if [[ -z "$INPUT" ]]; then
    error "No input file specified. Use -h for help."
fi

if [[ ! -f "$INPUT" ]]; then
    error "Input file not found: $INPUT"
fi

if [[ ! -f "$PASS_LIB" ]]; then
    error "RockClimb pass library not found: $PASS_LIB\nBuild with: cd passes/build && cmake .. && make"
fi

if [[ ! -f "$CONFIG" ]]; then
    error "Config file not found: $CONFIG"
fi

# Determine output base name
if [[ -z "$OUTPUT" ]]; then
    OUTPUT=$(basename "$INPUT" .c)
fi

# Create output directory if needed
OUTPUT_DIR=$(dirname "$OUTPUT")
if [[ -n "$OUTPUT_DIR" && "$OUTPUT_DIR" != "." ]]; then
    mkdir -p "$OUTPUT_DIR"
fi

# Temporary directory for intermediate files
TMP_DIR=$(mktemp -d)
trap "rm -rf $TMP_DIR" EXIT

echo "=========================================="
echo "RockClimb MSP430 Compiler"
echo "=========================================="
echo "Input:    $INPUT"
echo "Output:   $OUTPUT.{s,o}"
echo "Config:   $CONFIG"
echo "Memory checkpointing: $MEMORY_CKPT"
echo ""

# Step 1: Compile C to LLVM IR (targeting MSP430)
info "Step 1: Compiling C to MSP430 LLVM IR..."
"$CLANG" --target=msp430-elf \
    -S -emit-llvm \
    -O0 -Xclang -disable-O0-optnone \
    -I"$ROCKCLIMB_DIR/passes/include" \
    $EXTRA_INCLUDES \
    "$INPUT" -o "$TMP_DIR/input.ll" 2>&1

if [[ "$DEBUG" == "true" ]]; then
    echo "  Generated IR: $TMP_DIR/input.ll"
    echo "  Lines: $(wc -l < "$TMP_DIR/input.ll")"
fi

# Step 2: Run RockClimb pass
info "Step 2: Running RockClimb pass..."
PASS_OUTPUT=$("$OPT" -load-pass-plugin="$PASS_LIB" \
    -passes=rockclimb \
    -rockclimb-config="$CONFIG" \
    -rockclimb-memory-ckpt="$MEMORY_CKPT" \
    -S "$TMP_DIR/input.ll" -o "$TMP_DIR/instrumented.ll" 2>&1)

if [[ "$DEBUG" == "true" ]]; then
    echo "$PASS_OUTPUT"
else
    # Show summary
    echo "$PASS_OUTPUT" | grep -E "^(Region|Memory|Inserted|===)" | head -10
fi

# Count checkpoints
NUM_BOUNDARIES=$(echo "$PASS_OUTPUT" | grep -c "Region starting at" || echo "0")
NUM_MEM_CKPTS=$(echo "$PASS_OUTPUT" | grep "Memory checkpoints" | grep -oE '[0-9]+' | head -1 || echo "0")

echo ""
echo "  Regions: $NUM_BOUNDARIES"
echo "  Memory checkpoints: ${NUM_MEM_CKPTS:-0}"

# Step 3: Compile to MSP430 assembly
info "Step 3: Compiling to MSP430 assembly..."
"$LLC" -march=msp430 -O"$OPT_LEVEL" \
    "$TMP_DIR/instrumented.ll" -o "$OUTPUT.s" 2>&1

ASM_LINES=$(wc -l < "$OUTPUT.s")
echo "  Generated: $OUTPUT.s ($ASM_LINES lines)"

# Step 4: Compile to object file (unless asm-only)
if [[ "$ASM_ONLY" != "true" ]]; then
    info "Step 4: Compiling to object file..."
    "$LLC" -march=msp430 -O"$OPT_LEVEL" -filetype=obj \
        "$TMP_DIR/instrumented.ll" -o "$OUTPUT.o" 2>&1

    OBJ_SIZE=$(stat -f%z "$OUTPUT.o" 2>/dev/null || stat -c%s "$OUTPUT.o" 2>/dev/null)
    echo "  Generated: $OUTPUT.o ($OBJ_SIZE bytes)"
fi

# Analysis
if [[ "$ANALYZE" == "true" ]]; then
    echo ""
    echo "=========================================="
    info "Analysis"
    echo "=========================================="

    echo ""
    echo "NVM Section Symbols:"
    "$OBJDUMP" -t "$OUTPUT.o" 2>/dev/null | grep -E "\.nvm|__nvm" | while read line; do
        echo "  $line"
    done

    echo ""
    echo "Section Sizes:"
    "$OBJDUMP" -h "$OUTPUT.o" 2>/dev/null | grep -E "^\s+[0-9]" | awk '{printf "  %-20s %s bytes\n", $2, $3}'

    echo ""
    echo "External Dependencies:"
    "$OBJDUMP" -t "$OUTPUT.o" 2>/dev/null | grep "\*UND\*" | awk '{print "  " $NF}'
fi

echo ""
echo "=========================================="
success "Compilation complete!"
echo "=========================================="
echo ""
echo "Output files:"
echo "  Assembly:     $OUTPUT.s"
if [[ "$ASM_ONLY" != "true" ]]; then
    echo "  Object:       $OUTPUT.o"
fi
echo "  Instrumented: $TMP_DIR/instrumented.ll (temp)"
echo ""
echo "To examine the assembly:"
echo "  cat $OUTPUT.s"
echo ""
echo "To examine the object file:"
echo "  $OBJDUMP -d $OUTPUT.o    # Disassembly"
echo "  $OBJDUMP -t $OUTPUT.o    # Symbol table"
echo "  $OBJDUMP -h $OUTPUT.o    # Section headers"

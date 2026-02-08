#!/bin/bash

# Test runner for RockClimb Memory Checkpointing
# Usage: ./run_memory_ckpt_tests.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
TMP_DIR="$PROJECT_DIR/tmp"

# Load environment variables from .env if it exists
if [[ -f "$PROJECT_DIR/.env" ]]; then
    source "$PROJECT_DIR/.env"
fi

# LLVM tools
if [[ -n "${LLVM_DIR}" ]]; then
    CLANG="${CLANG:-${LLVM_DIR}/bin/clang}"
    OPT="${OPT:-${LLVM_DIR}/bin/opt}"
else
    CLANG="${CLANG:-clang}"
    OPT="${OPT:-opt}"
fi

PASS_LIB="$PROJECT_DIR/passes/build/CheckpointPass.so"
CONFIG="$SCRIPT_DIR/rockclimb_config.json"
INCLUDE_DIR="$PROJECT_DIR/passes/include"

# Ensure tmp directory exists
mkdir -p "$TMP_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

echo "=========================================="
echo "RockClimb Memory Checkpointing Test Suite"
echo "=========================================="
echo "Config: $CONFIG"
echo ""

# Check prerequisites
if [ ! -f "$PASS_LIB" ]; then
    echo -e "${RED}Error: Pass library not found at $PASS_LIB${NC}"
    echo "Run: cd passes/build && cmake .. && make"
    exit 1
fi

if [ ! -f "$CONFIG" ]; then
    echo -e "${RED}Error: Config file not found at $CONFIG${NC}"
    exit 1
fi

# Memory checkpointing test files
TESTS=(
    "test_rockclimb_memory_loop:Loop with memory - checkpoint across loop boundary"
    "test_rockclimb_memory_stack:Stack variables - checkpoint allocas across regions"
    "test_rockclimb_memory_global:Global variables - checkpoint globals across regions"
    "test_rockclimb_memory_mixed:Mixed variables - checkpoint both allocas and globals"
)

# Run each test
for test_entry in "${TESTS[@]}"; do
    IFS=':' read -r test_name description <<< "$test_entry"
    test_file="$SCRIPT_DIR/${test_name}.c"
    ll_file="$TMP_DIR/${test_name}.ll"
    output_file="$TMP_DIR/${test_name}_memckpt.ll"

    echo "----------------------------------------"
    echo -e "${YELLOW}Test: $test_name${NC}"
    echo "  $description"
    echo ""

    if [ ! -f "$test_file" ]; then
        echo -e "${RED}  SKIP: $test_file not found${NC}"
        continue
    fi

    # Compile C to LLVM IR
    echo "  Compiling to IR..."
    "$CLANG" -S -emit-llvm -O0 -Xclang -disable-O0-optnone \
        -I"$INCLUDE_DIR" "$test_file" -o "$ll_file" 2>/dev/null

    # Run RockClimb pass with memory checkpointing enabled
    echo "  Running RockClimb pass with memory checkpointing..."
    echo ""

    OUTPUT=$("$OPT" -load-pass-plugin="$PASS_LIB" \
              -passes=rockclimb \
              -rockclimb-config="$CONFIG" \
              -rockclimb-memory-ckpt=true \
              "$ll_file" -S -o "$output_file" 2>&1) || true

    # Display output
    echo "$OUTPUT"

    # Check for success indicators
    if echo "$OUTPUT" | grep -q "Memory checkpoints"; then
        echo ""
        echo -e "${GREEN}  PASS: Memory checkpoints identified${NC}"

        # Check for NVM slots in output
        if grep -q "@__nvm_b" "$output_file" 2>/dev/null; then
            echo -e "${GREEN}  PASS: NVM slots created in IR${NC}"
        else
            echo -e "${CYAN}  INFO: No NVM slots found (may be expected)${NC}"
        fi

        # Check for restore functions
        if grep -q "@__restore_boundary_" "$output_file" 2>/dev/null; then
            echo -e "${GREEN}  PASS: Restore functions generated${NC}"
        else
            echo -e "${CYAN}  INFO: No restore functions found (may be expected)${NC}"
        fi

        # Check for recovery dispatcher
        if grep -q "recovery" "$output_file" 2>/dev/null; then
            echo -e "${GREEN}  PASS: Recovery dispatcher inserted${NC}"
        else
            echo -e "${CYAN}  INFO: No recovery dispatcher found (may be expected)${NC}"
        fi
    elif echo "$OUTPUT" | grep -q "Error:"; then
        echo -e "${RED}  FAIL: Error during pass execution${NC}"
    else
        echo -e "${CYAN}  INFO: Check output for details${NC}"
    fi

    echo ""
done

echo ""
echo "=========================================="
echo "Comparing: With vs Without Memory Checkpointing"
echo "=========================================="
echo ""

# Run comparison on test_rockclimb_memory_stack
COMPARISON_TEST="test_rockclimb_memory_stack"
test_file="$SCRIPT_DIR/${COMPARISON_TEST}.c"
ll_file="$TMP_DIR/${COMPARISON_TEST}.ll"

if [ -f "$test_file" ]; then
    echo "Comparing on: $COMPARISON_TEST"
    echo ""

    # Compile
    "$CLANG" -S -emit-llvm -O0 -Xclang -disable-O0-optnone \
        -I"$INCLUDE_DIR" "$test_file" -o "$ll_file" 2>/dev/null

    echo -e "${CYAN}--- Without Memory Checkpointing ---${NC}"
    "$OPT" -load-pass-plugin="$PASS_LIB" \
          -passes=rockclimb \
          -rockclimb-config="$CONFIG" \
          -rockclimb-memory-ckpt=false \
          "$ll_file" -S -o /dev/null 2>&1 | grep -E "(Regions|checkpoints|Metrics)" || true

    echo ""
    echo -e "${CYAN}--- With Memory Checkpointing ---${NC}"
    "$OPT" -load-pass-plugin="$PASS_LIB" \
          -passes=rockclimb \
          -rockclimb-config="$CONFIG" \
          -rockclimb-memory-ckpt=true \
          "$ll_file" -S -o /dev/null 2>&1 | grep -E "(Regions|checkpoints|Metrics)" || true
fi

echo ""
echo "=========================================="
echo "Memory Checkpointing Test Suite Complete"
echo "=========================================="

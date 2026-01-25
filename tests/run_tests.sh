#!/bin/bash

# Test runner for checkpoint optimization validation
# Usage: ./run_tests.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
TMP_DIR="$PROJECT_DIR/tmp"

# Load environment variables from .env if it exists
if [[ -f "$PROJECT_DIR/.env" ]]; then
    source "$PROJECT_DIR/.env"
fi

# LLVM tools (use LLVM_DIR from .env, env vars, or fall back to PATH)
if [[ -n "${LLVM_DIR}" ]]; then
    CLANG="${CLANG:-${LLVM_DIR}/bin/clang}"
    OPT="${OPT:-${LLVM_DIR}/bin/opt}"
else
    CLANG="${CLANG:-clang}"
    OPT="${OPT:-opt}"
fi

PASS_LIB="$PROJECT_DIR/passes/build/CheckpointPass.so"
CONFIG="$SCRIPT_DIR/simple_config.json"

# Ensure tmp directory exists
mkdir -p "$TMP_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=========================================="
echo "Checkpoint Optimization Test Suite"
echo "=========================================="
echo "Config: $CONFIG"
echo "Capacity: 100, All costs: 1"
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

# Test files
TESTS=(
    "test_linear:Linear sequence - basic energy propagation"
    "test_diamond:Diamond CFG - asymmetric if-else paths"
    "test_simple_loop:Simple loop - frequency weighting"
    "test_nested_loops:Nested loops - avoid inner loop"
    "test_early_return:Early return - multiple exit blocks"
    "test_exit_constraint:Exit constraint - expensive exit block"
    "test_switch:Switch statement - multiple successors"
    "test_infeasible:Infeasible - block exceeds capacity (expect error)"
)

# Run each test
for test_entry in "${TESTS[@]}"; do
    IFS=':' read -r test_name description <<< "$test_entry"
    test_file="$SCRIPT_DIR/${test_name}.c"
    ll_file="$TMP_DIR/${test_name}.ll"

    echo "----------------------------------------"
    echo -e "${YELLOW}Test: $test_name${NC}"
    echo "  $description"
    echo ""

    if [ ! -f "$test_file" ]; then
        echo -e "${RED}  SKIP: $test_file not found${NC}"
        continue
    fi

    # Compile C to LLVM IR (use -O3 for aggressive optimization)
    echo "  Compiling to IR..."
    "$CLANG" -S -emit-llvm -O3 "$test_file" -o "$ll_file" 2>/dev/null

    # Run checkpoint pass
    echo "  Running checkpoint pass..."
    echo ""

    if [[ "$test_name" == "test_infeasible" ]]; then
        # Expect this test to report "exceed capacity" error
        OUTPUT=$("$OPT" -load-pass-plugin="$PASS_LIB" \
                  -passes=checkpoint \
                  -energy-config="$CONFIG" \
                  "$ll_file" -S -o /dev/null 2>&1)
        echo "$OUTPUT"
        if echo "$OUTPUT" | grep -q "exceed energy capacity"; then
            echo -e "${GREEN}  EXPECTED: Infeasibility detected${NC}"
        else
            echo -e "${RED}  UNEXPECTED: Should have reported capacity error${NC}"
        fi
    else
        # Normal test - should succeed
        if "$OPT" -load-pass-plugin="$PASS_LIB" \
                  -passes=checkpoint \
                  -energy-config="$CONFIG" \
                  "$ll_file" -S -o /dev/null 2>&1; then
            echo -e "${GREEN}  PASS${NC}"
        else
            echo -e "${RED}  FAIL${NC}"
        fi
    fi

    echo ""
done

echo "=========================================="
echo "Test suite complete"
echo "=========================================="

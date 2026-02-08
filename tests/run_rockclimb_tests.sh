#!/bin/bash

# Test runner for RockClimb pass validation
# Usage: ./run_rockclimb_tests.sh

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
CONFIG="$SCRIPT_DIR/rockclimb_config.json"
MILP_CONFIG="$PROJECT_DIR/benchmarks/sample_milp_config.json"

# Ensure tmp directory exists
mkdir -p "$TMP_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

echo "=========================================="
echo "RockClimb Pass Test Suite"
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

# Test files for RockClimb
TESTS=(
    "test_rockclimb_linear:Linear sequence - basic region partitioning"
    "test_rockclimb_loop:Simple loop - mandatory loop header boundary"
    "test_rockclimb_nested:Nested loops - multiple loop boundaries"
    "test_rockclimb_diamond:Diamond CFG - branching within regions"
    "test_rockclimb_liveout:Live-out registers - distributed checkpointing"
)

# Run each test
for test_entry in "${TESTS[@]}"; do
    IFS=':' read -r test_name description <<< "$test_entry"
    test_file="$SCRIPT_DIR/${test_name}.c"
    ll_file="$TMP_DIR/${test_name}.ll"
    output_file="$TMP_DIR/${test_name}_rockclimb.ll"

    echo "----------------------------------------"
    echo -e "${YELLOW}Test: $test_name${NC}"
    echo "  $description"
    echo ""

    if [ ! -f "$test_file" ]; then
        echo -e "${RED}  SKIP: $test_file not found${NC}"
        continue
    fi

    # Compile C to LLVM IR (use -O0 with disable-O0-optnone for finer basic blocks)
    echo "  Compiling to IR..."
    "$CLANG" -S -emit-llvm -O0 -Xclang -disable-O0-optnone "$test_file" -o "$ll_file" 2>/dev/null

    # Run RockClimb pass
    echo "  Running RockClimb pass..."
    echo ""

    OUTPUT=$("$OPT" -load-pass-plugin="$PASS_LIB" \
              -passes=rockclimb \
              -rockclimb-config="$CONFIG" \
              "$ll_file" -S -o "$output_file" 2>&1) || true

    # Display output
    echo "$OUTPUT"

    # Check for success indicators
    if echo "$OUTPUT" | grep -q "Region boundaries"; then
        echo -e "${GREEN}  PASS: Regions created successfully${NC}"
    elif echo "$OUTPUT" | grep -q "Error:"; then
        echo -e "${RED}  FAIL: Error during pass execution${NC}"
    else
        echo -e "${CYAN}  INFO: Check output for details${NC}"
    fi

    echo ""
done

echo ""
echo "=========================================="
echo "Comparison: RockClimb vs MILP"
echo "=========================================="
echo ""

# Run comparison on test_linear (use existing test that both passes can handle)
COMPARISON_TEST="test_linear"
test_file="$SCRIPT_DIR/${COMPARISON_TEST}.c"
ll_file="$TMP_DIR/${COMPARISON_TEST}.ll"

if [ -f "$test_file" ]; then
    echo "Comparing passes on: $COMPARISON_TEST"
    echo ""

    # Compile
    "$CLANG" -S -emit-llvm -O0 -Xclang -disable-O0-optnone "$test_file" -o "$ll_file" 2>/dev/null

    echo -e "${CYAN}--- MILP Pass ---${NC}"
    "$OPT" -load-pass-plugin="$PASS_LIB" \
          -passes=milp \
          -energy-config="$SCRIPT_DIR/simple_config.json" \
          -milp-config="$MILP_CONFIG" \
          "$ll_file" -S -o /dev/null 2>&1 || true

    echo ""
    echo -e "${CYAN}--- RockClimb Pass ---${NC}"
    "$OPT" -load-pass-plugin="$PASS_LIB" \
          -passes=rockclimb \
          -rockclimb-config="$CONFIG" \
          "$ll_file" -S -o /dev/null 2>&1 || true
fi

echo ""
echo "=========================================="
echo "Test suite complete"
echo "=========================================="

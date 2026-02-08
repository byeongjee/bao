#!/bin/bash

# Unified test runner for checkpoint optimization passes
# Usage: ./run_tests.sh [--milp] [--rockclimb]
#   No flags  = run all tests (MILP + RockClimb + comparison)
#   --milp    = run only MILP tests
#   --rockclimb = run only RockClimb tests + comparison

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
MILP_CONFIG="$SCRIPT_DIR/simple_config.json"
ROCKCLIMB_CONFIG="$SCRIPT_DIR/rockclimb_config.json"

# Ensure tmp directory exists
mkdir -p "$TMP_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Parse flags
RUN_MILP=false
RUN_ROCKCLIMB=false

for arg in "$@"; do
    case "$arg" in
        --milp) RUN_MILP=true ;;
        --rockclimb) RUN_ROCKCLIMB=true ;;
        *) echo "Unknown option: $arg"; echo "Usage: $0 [--milp] [--rockclimb]"; exit 1 ;;
    esac
done

# Default: run everything
if ! $RUN_MILP && ! $RUN_ROCKCLIMB; then
    RUN_MILP=true
    RUN_ROCKCLIMB=true
fi

# Check prerequisites
if [ ! -f "$PASS_LIB" ]; then
    echo -e "${RED}Error: Pass library not found at $PASS_LIB${NC}"
    echo "Run: cd passes/build && cmake .. && make"
    exit 1
fi

# Test entries: name:description:pass:config:clang_opt:check_type
#   pass       = checkpoint | rockclimb
#   config     = config filename (relative to SCRIPT_DIR)
#   clang_opt  = O3 | O0 (O0 adds -Xclang -disable-O0-optnone)
#   check_type = pass (expect success) | infeasible (expect capacity error) | regions (check "Region boundaries")

MILP_TESTS=(
    "test_linear:Linear sequence - basic energy propagation:checkpoint:simple_config.json:O3:pass"
    "test_diamond:Diamond CFG - asymmetric if-else paths:checkpoint:simple_config.json:O3:pass"
    "test_simple_loop:Simple loop - frequency weighting:checkpoint:simple_config.json:O3:pass"
    "test_nested_loops:Nested loops - avoid inner loop:checkpoint:simple_config.json:O3:pass"
    "test_early_return:Early return - multiple exit blocks:checkpoint:simple_config.json:O3:pass"
    "test_exit_constraint:Exit constraint - expensive exit block:checkpoint:simple_config.json:O3:pass"
    "test_switch:Switch statement - multiple successors:checkpoint:simple_config.json:O3:pass"
    "test_infeasible:Infeasible - block exceeds capacity (expect error):checkpoint:simple_config.json:O3:infeasible"
)

ROCKCLIMB_TESTS=(
    "test_rockclimb_linear:Linear sequence - basic region partitioning:rockclimb:rockclimb_config.json:O0:regions"
    "test_rockclimb_loop:Simple loop - mandatory loop header boundary:rockclimb:rockclimb_config.json:O0:regions"
    "test_rockclimb_nested:Nested loops - multiple loop boundaries:rockclimb:rockclimb_config.json:O0:regions"
    "test_rockclimb_diamond:Diamond CFG - branching within regions:rockclimb:rockclimb_config.json:O0:regions"
    "test_rockclimb_liveout:Live-out registers - distributed checkpointing:rockclimb:rockclimb_config.json:O0:regions"
)

# Collect tests to run
TESTS=()
if $RUN_MILP; then
    TESTS+=("${MILP_TESTS[@]}")
fi
if $RUN_ROCKCLIMB; then
    TESTS+=("${ROCKCLIMB_TESTS[@]}")
fi

# Header
echo "=========================================="
echo "Checkpoint Optimization Test Suite"
echo "=========================================="
if $RUN_MILP && $RUN_ROCKCLIMB; then
    echo "Mode: All tests (MILP + RockClimb)"
elif $RUN_MILP; then
    echo "Mode: MILP tests only"
else
    echo "Mode: RockClimb tests only"
fi
echo ""

# Verify config files exist
CONFIGS_OK=true
if $RUN_MILP && [ ! -f "$MILP_CONFIG" ]; then
    echo -e "${RED}Error: MILP config not found at $MILP_CONFIG${NC}"
    CONFIGS_OK=false
fi
if $RUN_ROCKCLIMB && [ ! -f "$ROCKCLIMB_CONFIG" ]; then
    echo -e "${RED}Error: RockClimb config not found at $ROCKCLIMB_CONFIG${NC}"
    CONFIGS_OK=false
fi
if ! $CONFIGS_OK; then
    exit 1
fi

# Run each test
run_test() {
    local test_name="$1"
    local description="$2"
    local pass="$3"
    local config="$4"
    local clang_opt="$5"
    local check_type="$6"

    local test_file="$SCRIPT_DIR/${test_name}.c"
    local ll_file="$TMP_DIR/${test_name}.ll"
    local config_path="$SCRIPT_DIR/${config}"

    echo "----------------------------------------"
    echo -e "${YELLOW}Test: $test_name${NC}"
    echo "  $description"
    echo ""

    if [ ! -f "$test_file" ]; then
        echo -e "${RED}  SKIP: $test_file not found${NC}"
        echo ""
        return
    fi

    # Compile C to LLVM IR
    echo "  Compiling to IR..."
    if [[ "$clang_opt" == "O0" ]]; then
        "$CLANG" -S -emit-llvm -O0 -Xclang -disable-O0-optnone "$test_file" -o "$ll_file" 2>/dev/null
    else
        "$CLANG" -S -emit-llvm -O3 "$test_file" -o "$ll_file" 2>/dev/null
    fi

    # Build pass-specific flags
    local pass_flag
    if [[ "$pass" == "checkpoint" ]]; then
        pass_flag="-energy-config=$config_path"
    else
        pass_flag="-rockclimb-config=$config_path"
    fi

    # Run pass
    echo "  Running $pass pass..."
    echo ""

    case "$check_type" in
        infeasible)
            OUTPUT=$("$OPT" -load-pass-plugin="$PASS_LIB" \
                      -passes="$pass" \
                      "$pass_flag" \
                      "$ll_file" -S -o /dev/null 2>&1)
            echo "$OUTPUT"
            if echo "$OUTPUT" | grep -q "exceed energy capacity"; then
                echo -e "${GREEN}  EXPECTED: Infeasibility detected${NC}"
            else
                echo -e "${RED}  UNEXPECTED: Should have reported capacity error${NC}"
            fi
            ;;
        regions)
            OUTPUT=$("$OPT" -load-pass-plugin="$PASS_LIB" \
                      -passes="$pass" \
                      "$pass_flag" \
                      "$ll_file" -S -o /dev/null 2>&1) || true
            echo "$OUTPUT"
            if echo "$OUTPUT" | grep -q "Region boundaries"; then
                echo -e "${GREEN}  PASS: Regions created successfully${NC}"
            elif echo "$OUTPUT" | grep -q "Error:"; then
                echo -e "${RED}  FAIL: Error during pass execution${NC}"
            else
                echo -e "${CYAN}  INFO: Check output for details${NC}"
            fi
            ;;
        pass)
            if "$OPT" -load-pass-plugin="$PASS_LIB" \
                      -passes="$pass" \
                      "$pass_flag" \
                      "$ll_file" -S -o /dev/null 2>&1; then
                echo -e "${GREEN}  PASS${NC}"
            else
                echo -e "${RED}  FAIL${NC}"
            fi
            ;;
    esac

    echo ""
}

# Execute all selected tests
for test_entry in "${TESTS[@]}"; do
    IFS=':' read -r name desc pass config clang_opt check_type <<< "$test_entry"
    run_test "$name" "$desc" "$pass" "$config" "$clang_opt" "$check_type"
done

# Run comparison if RockClimb tests are included
if $RUN_ROCKCLIMB; then
    echo ""
    echo "=========================================="
    echo "Comparison: RockClimb vs MILP"
    echo "=========================================="
    echo ""

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
              -passes=checkpoint \
              -energy-config="$MILP_CONFIG" \
              "$ll_file" -S -o /dev/null 2>&1 || true

        echo ""
        echo -e "${CYAN}--- RockClimb Pass ---${NC}"
        "$OPT" -load-pass-plugin="$PASS_LIB" \
              -passes=rockclimb \
              -rockclimb-config="$ROCKCLIMB_CONFIG" \
              "$ll_file" -S -o /dev/null 2>&1 || true
    fi
fi

echo ""
echo "=========================================="
echo "Test suite complete"
echo "=========================================="

#!/bin/bash

# Test: PHI-defined live-out values must be checkpointed by RockClimb
#
# Compiles a test function to IR, promotes to SSA (creating PHI nodes),
# instruments with RockClimb, links with a runtime driver, and executes.
# The driver simulates power failure and checks whether the PHI-defined
# value was saved to NVM.  If not, the restored result is wrong → FAIL.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
TMP_DIR="$PROJECT_DIR/tmp"

# Load environment variables from .env if it exists
if [[ -f "$PROJECT_DIR/.env" ]]; then
    source "$PROJECT_DIR/.env"
fi

# LLVM tools (build clang for IR generation/opt, system clang for native compilation)
if [[ -n "${LLVM_DIR}" ]]; then
    CLANG="${CLANG:-${LLVM_DIR}/bin/clang}"
    OPT="${OPT:-${LLVM_DIR}/bin/opt}"
else
    CLANG="${CLANG:-clang}"
    OPT="${OPT:-opt}"
fi
# System compiler for native C compilation (needs SDK headers)
SYS_CC="${SYS_CC:-/usr/bin/clang}"

PASS_LIB="$PROJECT_DIR/passes/build/CheckpointPass.so"
ESTIMATOR_CONFIG="$SCRIPT_DIR/estimator_ir_weighted.json"
ROCKCLIMB_CONFIG="$SCRIPT_DIR/rockclimb_params.json"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

mkdir -p "$TMP_DIR"

TEST_C="$SCRIPT_DIR/test_rockclimb_phi_liveout.c"
DRIVER_C="$SCRIPT_DIR/test_rockclimb_phi_liveout_driver.c"
RAW_LL="$TMP_DIR/phi_liveout_raw.ll"
SSA_LL="$TMP_DIR/phi_liveout_ssa.ll"
OUT_LL="$TMP_DIR/phi_liveout_out.ll"
OUT_OBJ="$TMP_DIR/phi_liveout_out.o"
DRIVER_OBJ="$TMP_DIR/phi_liveout_driver.o"
EXECUTABLE="$TMP_DIR/phi_liveout_test"

echo "=== Test: PHI-defined live-out checkpointing (runtime) ==="
echo ""

# Step 1: Compile test function to LLVM IR
echo "1. Compiling test to LLVM IR..."
"$CLANG" -S -emit-llvm -O0 -Xclang -disable-O0-optnone "$TEST_C" -o "$RAW_LL"

# Step 2: Promote to SSA to get PHI nodes
echo "2. Promoting to SSA (mem2reg)..."
"$OPT" -passes=mem2reg -S "$RAW_LL" -o "$SSA_LL"

# Step 3: Verify PHI nodes exist
echo "3. Checking for PHI nodes..."
if grep -q "= phi " "$SSA_LL"; then
    echo "   OK: PHI nodes found"
else
    echo -e "   ${RED}FAIL: No PHI nodes -- test is invalid${NC}"
    exit 1
fi

# Step 4: Run RockClimb pass
echo "4. Running RockClimb pass..."
"$OPT" -load-pass-plugin="$PASS_LIB" \
    -passes=rockclimb \
    -energy-config="$ESTIMATOR_CONFIG" \
    -rockclimb-config="$ROCKCLIMB_CONFIG" \
    -S "$SSA_LL" -o "$OUT_LL" 2>&1

# Step 5: Compile instrumented IR to object
echo "5. Compiling instrumented IR to object..."
"$SYS_CC" -c "$OUT_LL" -o "$OUT_OBJ"

# Step 6: Compile runtime driver
echo "6. Compiling runtime driver..."
"$SYS_CC" -c "$DRIVER_C" -o "$DRIVER_OBJ"

# Step 7: Link
echo "7. Linking..."
"$SYS_CC" "$OUT_OBJ" "$DRIVER_OBJ" -o "$EXECUTABLE"

# Step 8: Run
echo "8. Running test executable..."
echo ""
if "$EXECUTABLE"; then
    echo ""
    echo -e "${GREEN}TEST PASSED${NC}"
    exit 0
else
    echo ""
    echo -e "${RED}TEST FAILED${NC}"
    exit 1
fi

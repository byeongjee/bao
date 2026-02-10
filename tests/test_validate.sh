#!/bin/bash
#
# Generic energy validation test
# Tests both passing and failing cases with user-defined checkpoints.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASS_LIB="$PROJECT_DIR/passes/build/CheckpointPass.so"
CONFIG="$SCRIPT_DIR/simple_config.json"

if [[ ! -f "$PASS_LIB" ]]; then
    echo -e "${RED}Error: Pass library not found at $PASS_LIB${NC}"
    echo "Run: cd passes/build && cmake .. && make"
    exit 1
fi

# Load environment
[[ -f "$PROJECT_DIR/.env" ]] && source "$PROJECT_DIR/.env"

CLANG="${CLANG:-${LLVM_DIR:+$LLVM_DIR/bin/}clang}"
OPT="${OPT:-${LLVM_DIR:+$LLVM_DIR/bin/}opt}"
RUNTIME="$PROJECT_DIR/passes/runtime/energy_validate_runtime.c"

echo "=========================================="
echo "Energy Validation Tests"
echo "=========================================="
echo ""

PASS_COUNT=0
FAIL_COUNT=0

# Helper: run a validation test
run_validate_test() {
    local test_name="$1"
    local test_file="$2"
    local expect_pass="$3"  # true or false
    local config="${4:-$CONFIG}"

    echo -e "${YELLOW}Test: $test_name${NC}"

    if [[ ! -f "$test_file" ]]; then
        echo -e "${RED}  SKIP: $test_file not found${NC}"
        echo ""
        return
    fi

    local tmp_dir
    tmp_dir=$(mktemp -d)
    trap "rm -rf $tmp_dir" RETURN

    # Sysroot for source-built clang on macOS
    local sysroot_flags=""
    if command -v xcrun &>/dev/null; then
        sysroot_flags="-isysroot $(xcrun --show-sdk-path)"
    fi

    # Compile to IR
    $CLANG $sysroot_flags -S -emit-llvm -O0 -Xclang -disable-O0-optnone \
        "$test_file" -o "$tmp_dir/test.ll" 2>/dev/null

    # Run energy-validate pass
    $OPT -load-pass-plugin="$PASS_LIB" \
        -passes=energy-validate \
        -energy-config="$config" \
        -validate-checkpoint-function=checkpoint \
        -S "$tmp_dir/test.ll" -o "$tmp_dir/validated.ll" 2>/dev/null

    # Compile and link with validation runtime
    $CLANG -O0 $sysroot_flags \
        "$tmp_dir/validated.ll" "$RUNTIME" -o "$tmp_dir/test_bin" 2>/dev/null

    # Run the test (suppress shell abort trap message)
    local exit_code=0
    ("$tmp_dir/test_bin" > "$tmp_dir/stdout.txt" 2>"$tmp_dir/stderr.txt") 2>/dev/null || exit_code=$?

    if [[ "$expect_pass" == "true" ]]; then
        if [[ $exit_code -eq 0 ]]; then
            echo -e "${GREEN}  PASS: Completed without violation${NC}"
            PASS_COUNT=$((PASS_COUNT + 1))
        else
            echo -e "${RED}  FAIL: Unexpected violation (exit=$exit_code)${NC}"
            cat "$tmp_dir/stderr.txt" | head -5
            FAIL_COUNT=$((FAIL_COUNT + 1))
        fi
    else
        if [[ $exit_code -ne 0 ]]; then
            if grep -q "ENERGY VIOLATION" "$tmp_dir/stderr.txt"; then
                echo -e "${GREEN}  PASS: Energy violation correctly detected${NC}"
                PASS_COUNT=$((PASS_COUNT + 1))
            else
                echo -e "${RED}  FAIL: Non-zero exit but no ENERGY VIOLATION message${NC}"
                FAIL_COUNT=$((FAIL_COUNT + 1))
            fi
        else
            echo -e "${RED}  FAIL: Expected violation but test passed${NC}"
            FAIL_COUNT=$((FAIL_COUNT + 1))
        fi
    fi
    echo ""
}

# Test 1: Passing case (checkpoint every 5 iterations)
run_validate_test "Conditional checkpoint (K=5, should pass)" \
    "$SCRIPT_DIR/test_validate_pass.c" "true"

# Test 2: Failing case (checkpoint every 50 iterations)
run_validate_test "Insufficient checkpoint (K=50, should fail)" \
    "$SCRIPT_DIR/test_validate_fail.c" "false"

# Summary
echo "=========================================="
echo -e "Results: ${GREEN}$PASS_COUNT passed${NC}, ${RED}$FAIL_COUNT failed${NC}"
echo "=========================================="

[[ $FAIL_COUNT -eq 0 ]] && exit 0 || exit 1

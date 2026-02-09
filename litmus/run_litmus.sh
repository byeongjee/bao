#!/bin/bash

# Litmus microbenchmark runner for MILP checkpoint optimizer
# Usage:
#   ./litmus/run_litmus.sh                     # run all litmus tests
#   ./litmus/run_litmus.sh litmus_forced_ckpt  # run one specific test
#   ./litmus/run_litmus.sh --list              # list available tests

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
OUT_DIR="$SCRIPT_DIR/out"

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
CONFIG="$SCRIPT_DIR/litmus_config.json"
TIGHT_CONFIG="$SCRIPT_DIR/litmus_tight_config.json"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# All litmus tests: name:config_file
# config_file is relative to SCRIPT_DIR
LITMUS_TESTS=(
    "litmus_no_ckpt:litmus_config.json"
    "litmus_forced_ckpt:litmus_config.json"
    "litmus_loop:litmus_config.json"
    "litmus_nested_loop:litmus_config.json"
    "litmus_diamond:litmus_config.json"
    "litmus_switch:litmus_config.json"
    "litmus_store_reg:litmus_config.json"
    "litmus_store_global:litmus_config.json"
    "litmus_vm_hot:litmus_config.json"
    "litmus_vm_overflow:litmus_config.json"
    "litmus_needvol:litmus_config.json"
    "litmus_tight:litmus_tight_config.json"
    "litmus_infeasible:litmus_config.json"
)

list_tests() {
    echo "Available litmus tests:"
    for entry in "${LITMUS_TESTS[@]}"; do
        IFS=':' read -r name config <<< "$entry"
        local src="$SCRIPT_DIR/${name}.c"
        # Extract the comment header (first line of /* ... */ comment)
        if [[ -f "$src" ]]; then
            local desc
            desc=$(head -1 "$src" | sed 's|^/\* *||; s| *\*/$||')
            echo "  $name  — $desc"
        else
            echo "  $name  (source not found)"
        fi
    done
}

run_litmus() {
    local test_name="$1"
    local config_file="$2"

    local src="$SCRIPT_DIR/${test_name}.c"
    local input_ll="$OUT_DIR/${test_name}.ll"
    local output_ll="$OUT_DIR/${test_name}_out.ll"
    local stderr_log="$OUT_DIR/${test_name}_stderr.txt"
    local config_path="$SCRIPT_DIR/${config_file}"

    echo -e "${BOLD}=========================================="
    echo -e "  ${test_name}"
    echo -e "==========================================${NC}"

    # Extract description from source comment
    if [[ -f "$src" ]]; then
        local desc
        desc=$(head -3 "$src" | sed 's|^/\* *||; s|^ \* *||; s| *\*/$||' | tr '\n' ' ')
        echo -e "${CYAN}  $desc${NC}"
    fi
    echo ""

    if [[ ! -f "$src" ]]; then
        echo -e "${RED}  SKIP: $src not found${NC}"
        echo ""
        return
    fi

    # Step 1: Compile C -> LLVM IR (O0, disable-O0-optnone), then run mem2reg
    local raw_ll="$OUT_DIR/${test_name}_raw.ll"
    echo -e "${YELLOW}--- Compiling to IR (O0 + mem2reg) ---${NC}"
    "$CLANG" -S -emit-llvm -O0 -Xclang -disable-O0-optnone "$src" -o "$raw_ll" 2>/dev/null
    "$OPT" -passes=mem2reg -S "$raw_ll" -o "$input_ll" 2>/dev/null
    echo "  $input_ll"
    echo ""

    # Step 2: Show input IR (after mem2reg, before checkpoint)
    echo -e "${BOLD}=== BEFORE (input IR) ===${NC}"
    cat "$input_ll"
    echo ""

    # Step 3: Run MILP checkpoint pass
    echo -e "${YELLOW}--- Running MILP checkpoint pass ---${NC}"
    echo "  config: $config_file"
    echo ""

    local pass_exit=0
    "$OPT" -load-pass-plugin="$PASS_LIB" \
           -passes=checkpoint \
           -energy-config="$config_path" \
           -S "$input_ll" -o "$output_ll" 2>"$stderr_log" || pass_exit=$?

    # Step 4: Show MILP solution (stderr output)
    echo -e "${BOLD}=== MILP SOLUTION ===${NC}"
    if [[ -s "$stderr_log" ]]; then
        cat "$stderr_log"
    else
        echo "  (no optimizer output)"
    fi
    echo ""

    # Step 5: Show output IR (or error)
    if [[ $pass_exit -ne 0 ]]; then
        echo -e "${RED}=== PASS FAILED (exit code $pass_exit) ===${NC}"
        if grep -q "exceed energy capacity" "$stderr_log" 2>/dev/null; then
            echo -e "${GREEN}  EXPECTED: Infeasibility detected${NC}"
        else
            echo -e "${RED}  UNEXPECTED failure${NC}"
        fi
    else
        echo -e "${BOLD}=== AFTER (instrumented IR) ===${NC}"
        cat "$output_ll"
        echo ""

        # Step 6: Show diff
        echo -e "${BOLD}=== DIFF ===${NC}"
        diff -u "$input_ll" "$output_ll" || true
    fi

    echo ""
    echo ""
}

# --- Main ---

# Check prerequisites
if [[ ! -f "$PASS_LIB" ]]; then
    echo -e "${RED}Error: Pass library not found at $PASS_LIB${NC}"
    echo "Run: cd passes/build && cmake .. && make"
    exit 1
fi

# Ensure output directory exists
mkdir -p "$OUT_DIR"

# Handle arguments
if [[ $# -eq 0 ]]; then
    # Run all tests
    echo "=========================================="
    echo "  Litmus Microbenchmark Suite"
    echo "=========================================="
    echo ""

    for entry in "${LITMUS_TESTS[@]}"; do
        IFS=':' read -r name config <<< "$entry"
        run_litmus "$name" "$config"
    done

    echo "=========================================="
    echo "  Litmus suite complete"
    echo "=========================================="
elif [[ "$1" == "--list" ]]; then
    list_tests
else
    # Run specific test(s)
    for test_name in "$@"; do
        # Find matching entry
        found=false
        for entry in "${LITMUS_TESTS[@]}"; do
            IFS=':' read -r name config <<< "$entry"
            if [[ "$name" == "$test_name" ]]; then
                run_litmus "$name" "$config"
                found=true
                break
            fi
        done
        if ! $found; then
            echo -e "${RED}Unknown test: $test_name${NC}"
            echo "Run with --list to see available tests."
            exit 1
        fi
    done
fi

#!/bin/bash

# Scenario runner for MILP checkpoint optimizer
# Usage:
#   ./scenario/run_scenario.sh                        # run all scenarios
#   ./scenario/run_scenario.sh scenario_forced_ckpt   # run one specific scenario
#   ./scenario/run_scenario.sh --list                 # list available scenarios

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
BB_FREQ_RUNTIME="$PROJECT_DIR/passes/runtime/bb_freq_runtime.c"
CONFIG="$SCRIPT_DIR/scenario_config.json"
TIGHT_CONFIG="$SCRIPT_DIR/scenario_tight_config.json"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# Sysroot for source-built clang on macOS (needed when linking instrumented binaries)
SYSROOT_FLAGS=""
if command -v xcrun &>/dev/null; then
    SYSROOT_FLAGS="-isysroot $(xcrun --show-sdk-path)"
fi

# All scenarios: name:energy_config:milp_config[:source_name]
# energy_config: energy estimator config (instruction costs)
# milp_config:   MILP optimization parameters (capacity, etc.)
# source_name:   optional; if provided, uses ${source_name}.c instead of ${name}.c
SCENARIOS=(
    "scenario_no_ckpt:scenario_config.json:scenario_milp_config.json"
    "scenario_forced_ckpt:scenario_config.json:scenario_milp_config.json"
    "scenario_loop:scenario_config.json:scenario_milp_config.json"
    "scenario_nested_loop:scenario_config.json:scenario_milp_config.json"
    "scenario_diamond:scenario_config.json:scenario_milp_config.json"
    "scenario_switch:scenario_config.json:scenario_milp_config.json"
    "scenario_store_reg:scenario_config.json:scenario_milp_config.json"
    "scenario_store_global:scenario_config.json:scenario_milp_config.json"
    "scenario_vm_hot:scenario_config.json:scenario_milp_config.json"
    "scenario_vm_overflow:scenario_vm_overflow_config.json:scenario_milp_vm_overflow_config.json"
    "scenario_vm_nvm_enforcement:scenario_config.json:scenario_milp_config.json"
    "scenario_needvol:scenario_config.json:scenario_milp_config.json"
    "scenario_tight:scenario_tight_config.json:scenario_milp_tight_config.json"
    "scenario_infeasible:scenario_config.json:scenario_milp_config.json"
    "scenario_nvm_efficient:scenario_config.json:scenario_milp_config.json"
    "scenario_noncandidate_ckpt:scenario_config.json:scenario_milp_config.json"
    "scenario_nvm_efficient_vm:scenario_nvm_efficient_vm_config.json:scenario_milp_nvm_efficient_vm_config.json:scenario_nvm_efficient"
    "scenario_alloca_ckpt:scenario_config.json:scenario_milp_config.json"
    "scenario_ssa_ckpt:scenario_config.json:scenario_milp_config.json"
    # Slide examples (descriptive names mapped to existing source files)
    "slide_two_phase_checkpoint:slide_config.json:slide_milp_config.json:slide_basic"
    "slide_liveout_commit_cost:slide_config.json:slide_milp_config.json:slide_distributed"
    "slide_hot_cold_nvm_placement:slide_config.json:slide_milp_nvm_config.json:slide_nvm"
    "slide_early_vs_late_boundary:slide_config.json:slide_milp_greedy_config.json:slide_greedy"
)

list_tests() {
    echo "Available scenarios:"
    for entry in "${SCENARIOS[@]}"; do
        IFS=':' read -r name econfig mconfig source <<< "$entry"
        local src="$SCRIPT_DIR/${source:-$name}.c"
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

run_scenario() {
    local test_name="$1"
    local energy_config_file="$2"
    local milp_config_file="$3"
    local source_name="${4:-$test_name}"

    local src="$SCRIPT_DIR/${source_name}.c"
    local input_ll="$OUT_DIR/${test_name}.ll"
    local output_ll="$OUT_DIR/${test_name}_out.ll"
    local stderr_log="$OUT_DIR/${test_name}_stderr.txt"
    local energy_config_path="$SCRIPT_DIR/${energy_config_file}"
    local milp_config_path="$SCRIPT_DIR/${milp_config_file}"

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

    # Step 1: Compile to LLVM IR (O0 + mem2reg), then collect BB frequencies.
    local raw_ll="$OUT_DIR/${test_name}_raw.ll"
    local freq_ll="$OUT_DIR/${test_name}_freq.ll"
    local freq_bin="$OUT_DIR/${test_name}_freq_run"
    local bb_freq_json="$OUT_DIR/${test_name}_bb_freq.json"
    echo -e "${YELLOW}--- Compiling to IR (O0 + mem2reg) ---${NC}"
    "$CLANG" $SYSROOT_FLAGS -S -emit-llvm -O0 -Xclang -disable-O0-optnone \
        "$src" -o "$raw_ll" 2>/dev/null
    "$OPT" -passes=mem2reg -S "$raw_ll" -o "$input_ll" 2>/dev/null
    echo "  $input_ll"
    echo ""

    # Step 1b: Collect BB frequencies via instrumentation
    echo -e "${YELLOW}--- Collecting BB frequencies ---${NC}"
    "$OPT" -load-pass-plugin="$PASS_LIB" \
           -passes=bb-freq-collect \
           -energy-config="$energy_config_path" \
           -milp-config="$milp_config_path" \
           -S "$input_ll" -o "$freq_ll" 2>/dev/null
    "$CLANG" $SYSROOT_FLAGS "$freq_ll" "$BB_FREQ_RUNTIME" -o "$freq_bin" 2>/dev/null
    (cd "$OUT_DIR" && "./${test_name}_freq_run") >/dev/null 2>&1 || true
    if [[ -f "$OUT_DIR/bb_freq.json" ]]; then
        mv "$OUT_DIR/bb_freq.json" "$bb_freq_json"
    fi
    echo "  $bb_freq_json"
    echo ""

    # Step 2: Show input IR (after mem2reg, before checkpoint)
    echo -e "${BOLD}=== BEFORE (input IR) ===${NC}"
    cat "$input_ll"
    echo ""

    # Step 3: Run MILP checkpoint pass
    echo -e "${YELLOW}--- Running MILP checkpoint pass ---${NC}"
    echo "  energy-config: $energy_config_file"
    echo "  milp-config:   $milp_config_file"
    echo ""

    local pass_exit=0
    "$OPT" -load-pass-plugin="$PASS_LIB" \
           -passes=checkpoint \
           -energy-config="$energy_config_path" \
           -milp-config="$milp_config_path" \
           -bb-freq-file="$bb_freq_json" \
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

if [[ ! -f "$BB_FREQ_RUNTIME" ]]; then
    echo -e "${RED}Error: BB frequency runtime not found at $BB_FREQ_RUNTIME${NC}"
    exit 1
fi

# Ensure output directory exists
mkdir -p "$OUT_DIR"

# Handle arguments
if [[ $# -eq 0 ]]; then
    # Run all scenarios
    echo "=========================================="
    echo "  Scenario Suite"
    echo "=========================================="
    echo ""

    for entry in "${SCENARIOS[@]}"; do
        IFS=':' read -r name econfig mconfig source <<< "$entry"
        run_scenario "$name" "$econfig" "$mconfig" "${source:-}"
    done

    echo "=========================================="
    echo "  Scenario suite complete"
    echo "=========================================="
elif [[ "$1" == "--list" ]]; then
    list_tests
else
    # Run specific scenario(s)
    for test_name in "$@"; do
        # Find matching entry
        found=false
        for entry in "${SCENARIOS[@]}"; do
            IFS=':' read -r name econfig mconfig source <<< "$entry"
            if [[ "$name" == "$test_name" ]]; then
                run_scenario "$name" "$econfig" "$mconfig" "${source:-}"
                found=true
                break
            fi
        done
        if ! $found; then
            echo -e "${RED}Unknown scenario: $test_name${NC}"
            echo "Run with --list to see available scenarios."
            exit 1
        fi
    done
fi

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
    LLVM_PROFDATA="${LLVM_PROFDATA:-${LLVM_DIR}/bin/llvm-profdata}"
else
    CLANG="${CLANG:-clang}"
    OPT="${OPT:-opt}"
    LLVM_PROFDATA="${LLVM_PROFDATA:-llvm-profdata}"
fi

PASS_LIB="$PROJECT_DIR/passes/build/CheckpointPass.so"

# Config files (split into estimator + mode-specific)
ESTIMATOR_UNIFORM="$SCRIPT_DIR/estimator_ir_uniform.json"
ESTIMATOR_WEIGHTED="$SCRIPT_DIR/estimator_ir_weighted.json"
MILP_PARAMS="$SCRIPT_DIR/milp_params.json"
MILP_PARAMS_SMALL="$SCRIPT_DIR/milp_params_small.json"
ROCKCLIMB_PARAMS="$SCRIPT_DIR/rockclimb_params.json"

# Ensure tmp directory exists
mkdir -p "$TMP_DIR"

# Sysroot for source-built clang on macOS (needed when linking profile runs)
SYSROOT_FLAGS=""
if command -v xcrun &>/dev/null; then
    SYSROOT_FLAGS="-isysroot $(xcrun --show-sdk-path)"
fi
PROFILE_CLANG="$CLANG"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Parse flags
RUN_MILP=false
RUN_ROCKCLIMB=false
RUN_VALIDATE=false

for arg in "$@"; do
    case "$arg" in
        --milp) RUN_MILP=true ;;
        --rockclimb) RUN_ROCKCLIMB=true ;;
        --validate) RUN_VALIDATE=true ;;
        *) echo "Unknown option: $arg"; echo "Usage: $0 [--milp] [--rockclimb] [--validate]"; exit 1 ;;
    esac
done

# Default: run everything
if ! $RUN_MILP && ! $RUN_ROCKCLIMB && ! $RUN_VALIDATE; then
    RUN_MILP=true
    RUN_ROCKCLIMB=true
    RUN_VALIDATE=true
fi

# Check prerequisites
if [ ! -f "$PASS_LIB" ]; then
    echo -e "${RED}Error: Pass library not found at $PASS_LIB${NC}"
    echo "Run: cd passes/build && cmake .. && make"
    exit 1
fi

if $RUN_MILP || $RUN_ROCKCLIMB; then
    if ! command -v "$LLVM_PROFDATA" >/dev/null 2>&1; then
        echo -e "${RED}Error: llvm-profdata not found ($LLVM_PROFDATA)${NC}"
        exit 1
    fi

    probe_dir=$(mktemp -d)
    cat >"$probe_dir/profile_probe.c" <<'EOF'
int main(void) { return 0; }
EOF
    selected_profile_clang=""
    candidate_clangs=("$PROFILE_CLANG" "$(command -v clang 2>/dev/null || true)" "/usr/bin/clang")
    for candidate in "${candidate_clangs[@]}"; do
        [[ -z "$candidate" ]] && continue
        if "$candidate" $SYSROOT_FLAGS \
            -fprofile-instr-generate="$probe_dir/profile_probe.profraw" \
            "$probe_dir/profile_probe.c" -o "$probe_dir/profile_probe_bin" >/dev/null 2>&1; then
            selected_profile_clang="$candidate"
            break
        fi
    done

    if [[ -z "$selected_profile_clang" ]]; then
        echo -e "${RED}Error: no clang toolchain can link -fprofile-instr-generate binaries${NC}"
        rm -rf "$probe_dir"
        exit 1
    fi

    PROFILE_CLANG="$selected_profile_clang"
    if [[ "$PROFILE_CLANG" != "$CLANG" ]]; then
        echo -e "${YELLOW}Info: using $PROFILE_CLANG for profile generation${NC}"
    fi
    rm -rf "$probe_dir"
fi

# Test entries: name:description:pass:estimator_config:milp_or_rc_config:clang_opt:check_type
#   pass            = checkpoint | rockclimb
#   estimator_config = estimator config filename (relative to SCRIPT_DIR)
#   milp_or_rc_config = MILP or RockClimb params config filename
#   clang_opt       = O3 | O0 (O0 adds -Xclang -disable-O0-optnone)
#   check_type      = pass (expect success) | infeasible (expect capacity error) |
#                     regions (check "Region boundaries") |
#                     missing_profile (expect hard profile-data error)

MILP_TESTS=(
    "test_linear:Linear sequence - basic energy propagation:checkpoint:estimator_ir_uniform.json:milp_params.json:O3:pass"
    "test_linear:MILP rejects non-profiled IR (expect hard error):checkpoint:estimator_ir_uniform.json:milp_params.json:O3:missing_profile"
    "test_diamond:Diamond CFG - asymmetric if-else paths:checkpoint:estimator_ir_uniform.json:milp_params.json:O3:pass"
    "test_simple_loop:Simple loop - frequency weighting:checkpoint:estimator_ir_uniform.json:milp_params.json:O3:pass"
    "test_nested_loops:Nested loops - avoid inner loop:checkpoint:estimator_ir_uniform.json:milp_params.json:O3:pass"
    "test_early_return:Early return - multiple exit blocks:checkpoint:estimator_ir_uniform.json:milp_params.json:O3:pass"
    "test_exit_constraint:Exit constraint - expensive exit block:checkpoint:estimator_ir_uniform.json:milp_params.json:O3:pass"
    "test_switch:Switch statement - multiple successors:checkpoint:estimator_ir_uniform.json:milp_params.json:O3:pass"
    "test_infeasible:Infeasible - block exceeds capacity (expect error):checkpoint:estimator_ir_uniform.json:milp_params_small.json:O3:infeasible"
    "test_distributed_stores:Distributed stores - checkpoint stores at def sites:checkpoint:estimator_ir_uniform.json:milp_params_small.json:O3:pass"
    "test_vm_nvm_placement:VM/NVM placement - global memory assignment:checkpoint:estimator_ir_uniform.json:milp_params_small.json:O3:pass"
)

ROCKCLIMB_TESTS=(
    "test_rockclimb_linear:Linear sequence - basic region partitioning:rockclimb:estimator_ir_weighted.json:rockclimb_params.json:O0:regions"
    "test_rockclimb_loop:Simple loop - mandatory loop header boundary:rockclimb:estimator_ir_weighted.json:rockclimb_params.json:O0:regions"
    "test_rockclimb_nested:Nested loops - multiple loop boundaries:rockclimb:estimator_ir_weighted.json:rockclimb_params.json:O0:regions"
    "test_rockclimb_diamond:Diamond CFG - branching within regions:rockclimb:estimator_ir_weighted.json:rockclimb_params.json:O0:regions"
    "test_rockclimb_liveout:Live-out registers - distributed checkpointing:rockclimb:estimator_ir_weighted.json:rockclimb_params.json:O0:regions"
    "test_rockclimb_phi_liveout:PHI-defined live-out - runtime checkpoint check:rockclimb:estimator_ir_weighted.json:rockclimb_params.json:O0:phi_runtime"
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
if $RUN_MILP && $RUN_ROCKCLIMB && $RUN_VALIDATE; then
    echo "Mode: All tests (MILP + RockClimb + Validate)"
elif $RUN_MILP && $RUN_ROCKCLIMB; then
    echo "Mode: MILP + RockClimb tests"
elif $RUN_MILP; then
    echo "Mode: MILP tests only"
elif $RUN_ROCKCLIMB; then
    echo "Mode: RockClimb tests only"
elif $RUN_VALIDATE; then
    echo "Mode: Validation tests only"
fi
echo ""

# Verify config files exist
CONFIGS_OK=true
if $RUN_MILP; then
    [ ! -f "$ESTIMATOR_UNIFORM" ] && echo -e "${RED}Error: Estimator config not found at $ESTIMATOR_UNIFORM${NC}" && CONFIGS_OK=false
    [ ! -f "$MILP_PARAMS" ] && echo -e "${RED}Error: MILP params not found at $MILP_PARAMS${NC}" && CONFIGS_OK=false
fi
if $RUN_ROCKCLIMB; then
    [ ! -f "$ESTIMATOR_WEIGHTED" ] && echo -e "${RED}Error: Estimator config not found at $ESTIMATOR_WEIGHTED${NC}" && CONFIGS_OK=false
    [ ! -f "$ROCKCLIMB_PARAMS" ] && echo -e "${RED}Error: RockClimb params not found at $ROCKCLIMB_PARAMS${NC}" && CONFIGS_OK=false
fi
if ! $CONFIGS_OK; then
    exit 1
fi

build_profiled_ir() {
    local src_file="$1"
    local out_ll="$2"
    local clang_opt="$3"
    local stem="$4"
    local work_dir="$TMP_DIR/profile_${stem}"

    mkdir -p "$work_dir"

    local profraw="$work_dir/${stem}.profraw"
    local profdata="$work_dir/${stem}.profdata"
    local train_bin="$work_dir/${stem}_train"
    local driver_file="$work_dir/${stem}_profile_driver.c"
    local opt_flag="-O3"
    local optnone_flags=()
    local train_inputs=("$src_file")

    if [[ "$clang_opt" == "O0" ]]; then
        opt_flag="-O0"
        optnone_flags=(-Xclang -disable-O0-optnone)
    fi

    if ! grep -qE "\\bmain[[:space:]]*\\(" "$src_file"; then
        python3 - "$src_file" "$stem" > "$driver_file" <<'PY'
import re
import sys

src_path, func_name = sys.argv[1], sys.argv[2]
text = open(src_path, "r", encoding="utf-8").read()
match = re.search(r"\b%s\s*\(([^)]*)\)\s*\{" % re.escape(func_name), text, re.S)
params = []
if match:
    raw = match.group(1).strip()
    if raw and raw != "void":
        params = [part.strip() for part in raw.split(",") if part.strip()]

decls = []
args = []
ptr_idx = 0
for i, param in enumerate(params):
    if "*" in param or "[" in param:
        ptr_idx += 1
        name = f"ptr_arg_{ptr_idx}"
        decls.append(f"    volatile int {name} = {ptr_idx};")
        args.append(f"&{name}")
    elif re.search(r"\b(float|double)\b", param):
        args.append("1.0")
    else:
        args.append(str(10 + i))

print(f"extern int {func_name}();")
print("int main(void) {")
for decl in decls:
    print(decl)
if args:
    print(f"    volatile int result = {func_name}({', '.join(args)});")
else:
    print(f"    volatile int result = {func_name}();")
print("    return (int)result;")
print("}")
PY
        train_inputs+=("$driver_file")
    fi

    "$PROFILE_CLANG" $SYSROOT_FLAGS "$opt_flag" "${optnone_flags[@]}" \
        -fprofile-instr-generate="$profraw" \
        "${train_inputs[@]}" -o "$train_bin" 2>/dev/null

    LLVM_PROFILE_FILE="$profraw" "$train_bin" >/dev/null 2>&1 || true

    if [[ ! -f "$profraw" ]]; then
        echo -e "${RED}  FAIL: profile run did not produce $profraw${NC}"
        return 1
    fi

    "$LLVM_PROFDATA" merge -o "$profdata" "$profraw" 2>/dev/null

    "$CLANG" $SYSROOT_FLAGS -S -emit-llvm "$opt_flag" "${optnone_flags[@]}" \
        -fprofile-instr-use="$profdata" \
        "$src_file" -o "$out_ll" 2>/dev/null
}

# Run each test
run_test() {
    local test_name="$1"
    local description="$2"
    local pass="$3"
    local estimator_config="$4"
    local mode_config="$5"
    local clang_opt="$6"
    local check_type="$7"

    local test_file="$SCRIPT_DIR/${test_name}.c"
    local ll_file="$TMP_DIR/${test_name}.ll"
    local estimator_path="$SCRIPT_DIR/${estimator_config}"
    local mode_config_path="$SCRIPT_DIR/${mode_config}"

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
    if [[ "$pass" == "checkpoint" && "$check_type" != "missing_profile" ]]; then
        echo "  Building profile data..."
        build_profiled_ir "$test_file" "$ll_file" "$clang_opt" "$test_name"
    else
        if [[ "$clang_opt" == "O0" ]]; then
            "$CLANG" -S -emit-llvm -O0 -Xclang -disable-O0-optnone \
                "$test_file" -o "$ll_file" 2>/dev/null
        else
            "$CLANG" -S -emit-llvm -O3 "$test_file" -o "$ll_file" 2>/dev/null
        fi
    fi

    # Build pass-specific flags
    local pass_flags
    if [[ "$pass" == "checkpoint" ]]; then
        pass_flags="-energy-config=$estimator_path -milp-config=$mode_config_path"
    else
        pass_flags="-energy-config=$estimator_path -rockclimb-config=$mode_config_path"
    fi

    # Run pass
    echo "  Running $pass pass..."
    echo ""

    case "$check_type" in
        infeasible)
            OUTPUT=$("$OPT" -load-pass-plugin="$PASS_LIB" \
                      -passes="$pass" \
                      $pass_flags \
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
                      $pass_flags \
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
                      $pass_flags \
                      "$ll_file" -S -o /dev/null 2>&1; then
                echo -e "${GREEN}  PASS${NC}"
            else
                echo -e "${RED}  FAIL${NC}"
            fi
            ;;
        missing_profile)
            set +e
            OUTPUT=$("$OPT" -load-pass-plugin="$PASS_LIB" \
                      -passes="$pass" \
                      $pass_flags \
                      "$ll_file" -S -o /dev/null 2>&1)
            EXIT_CODE=$?
            set -e
            echo "$OUTPUT"
            if [[ $EXIT_CODE -ne 0 ]] && \
               echo "$OUTPUT" | grep -q "profile-guided block frequencies"; then
                echo -e "${GREEN}  PASS: Missing profile correctly rejected${NC}"
            else
                echo -e "${RED}  FAIL: Expected hard failure for missing profile${NC}"
            fi
            ;;
        phi_runtime)
            # Runtime test: compile to IR, mem2reg, instrument, link with driver, run
            if [[ -x "$SCRIPT_DIR/${test_name}.sh" ]]; then
                if "$SCRIPT_DIR/${test_name}.sh"; then
                    echo -e "${GREEN}  PASS${NC}"
                else
                    echo -e "${RED}  FAIL${NC}"
                fi
            else
                echo -e "${RED}  SKIP: ${test_name}.sh not found or not executable${NC}"
            fi
            ;;
    esac

    echo ""
}

# Execute all selected tests
for test_entry in "${TESTS[@]}"; do
    IFS=':' read -r name desc pass est_config mode_config clang_opt check_type <<< "$test_entry"
    run_test "$name" "$desc" "$pass" "$est_config" "$mode_config" "$clang_opt" "$check_type"
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

        # Compile profiled IR for MILP comparison.
        build_profiled_ir "$test_file" "$ll_file" "O0" "${COMPARISON_TEST}_comparison"

        echo -e "${CYAN}--- MILP Pass ---${NC}"
        "$OPT" -load-pass-plugin="$PASS_LIB" \
              -passes=checkpoint \
              -energy-config="$ESTIMATOR_UNIFORM" \
              -milp-config="$MILP_PARAMS" \
              "$ll_file" -S -o /dev/null 2>&1 || true

        echo ""
        echo -e "${CYAN}--- RockClimb Pass ---${NC}"
        "$OPT" -load-pass-plugin="$PASS_LIB" \
              -passes=rockclimb \
              -energy-config="$ESTIMATOR_WEIGHTED" \
              -rockclimb-config="$ROCKCLIMB_PARAMS" \
              "$ll_file" -S -o /dev/null 2>&1 || true
    fi
fi

# Run validation tests if requested
if $RUN_VALIDATE; then
    echo ""
    echo "=========================================="
    echo "Energy Validation Tests"
    echo "=========================================="
    echo ""

    if [[ -x "$SCRIPT_DIR/test_validate.sh" ]]; then
        "$SCRIPT_DIR/test_validate.sh" || true
    else
        echo -e "${YELLOW}Skipping: test_validate.sh not found or not executable${NC}"
    fi
fi

echo ""
echo "=========================================="
echo "Test suite complete"
echo "=========================================="

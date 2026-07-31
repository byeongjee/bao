#!/bin/bash

# Unified test runner for checkpoint optimization passes
# Usage: ./run_tests.sh [--milp] [--rockclimb]
#   No flags  = run all tests (MILP + RockClimb)
#   --milp    = run only MILP tests
#   --rockclimb = run only RockClimb tests

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
TMP_DIR="$PROJECT_DIR/tmp"

# LLVM tools (use LLVM_DIR from environment or fall back to PATH)
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

# RockClimb coverage lives in pytest (test_rockclimb*.py); delegate.
if $RUN_ROCKCLIMB; then
    echo "Running RockClimb pytest suite (uv run pytest tests/ -m rockclimb)..."
    (cd "$PROJECT_DIR" && uv run pytest tests/ -m rockclimb)
    RUN_ROCKCLIMB=false
    if ! $RUN_MILP; then
        exit 0
    fi
fi

# Check prerequisites
if [ ! -f "$PASS_LIB" ]; then
    echo -e "${RED}Error: Pass library not found at $PASS_LIB${NC}"
    echo "Run: cd passes/build && cmake .. && make"
    exit 1
fi

if $RUN_ROCKCLIMB; then
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

BB_FREQ_RUNTIME="$PROJECT_DIR/passes/runtime/bb_freq_runtime.c"

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
    "test_linear:MILP rejects missing bb-freq-file (expect hard error):checkpoint:estimator_ir_uniform.json:milp_params.json:O3:missing_bb_freq"
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


# Collect tests to run
TESTS=()
if $RUN_MILP; then
    TESTS+=("${MILP_TESTS[@]}")
fi

# Header
echo "=========================================="
echo "Checkpoint Optimization Test Suite"
echo "=========================================="
if $RUN_MILP && $RUN_ROCKCLIMB; then
    echo "Mode: All tests (MILP + RockClimb)"
elif $RUN_MILP; then
    echo "Mode: MILP tests only"
elif $RUN_ROCKCLIMB; then
    echo "Mode: RockClimb tests only"
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
    local bb_freq_json=""
    if [[ "$pass" == "checkpoint" && "$check_type" != "missing_bb_freq" ]]; then
        # BB frequency workflow: compile at O0, annotate trip counts, collect frequencies
        "$CLANG" -S -emit-llvm -O0 -Xclang -disable-O0-optnone \
            "$test_file" -o "$ll_file" 2>/dev/null

        # Annotate trip counts
        "$OPT" -load-pass-plugin="$PASS_LIB" \
            -passes=tripcount-annotation \
            -S "$ll_file" -o "$TMP_DIR/${test_name}_ann.ll" 2>/dev/null

        echo "  Collecting BB frequencies..."
        local freq_work_dir="$TMP_DIR/freq_${test_name}"
        mkdir -p "$freq_work_dir"

        # Instrument for BB frequency collection
        "$OPT" -load-pass-plugin="$PASS_LIB" \
            -passes=bb-freq-collect \
            -energy-config="$estimator_path" \
            -milp-config="$mode_config_path" \
            -S "$TMP_DIR/${test_name}_ann.ll" -o "$freq_work_dir/freq_inst.ll" 2>/dev/null

        # Compile and run to get bb_freq.json
        "$CLANG" $SYSROOT_FLAGS -O0 \
            "$freq_work_dir/freq_inst.ll" "$BB_FREQ_RUNTIME" \
            -o "$freq_work_dir/freq_run" 2>/dev/null

        (cd "$freq_work_dir" && ./freq_run) >/dev/null 2>&1 || true

        if [[ ! -f "$freq_work_dir/bb_freq.json" ]]; then
            echo -e "${RED}  FAIL: BB frequency collection did not produce bb_freq.json${NC}"
            echo ""
            return
        fi
        bb_freq_json="$freq_work_dir/bb_freq.json"
        ll_file="$TMP_DIR/${test_name}_ann.ll"
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
        if [[ -n "$bb_freq_json" ]]; then
            pass_flags="$pass_flags -bb-freq-file=$bb_freq_json"
        fi
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
        missing_bb_freq)
            set +e
            OUTPUT=$("$OPT" -load-pass-plugin="$PASS_LIB" \
                      -passes="$pass" \
                      $pass_flags \
                      "$ll_file" -S -o /dev/null 2>&1)
            EXIT_CODE=$?
            set -e
            echo "$OUTPUT"
            if [[ $EXIT_CODE -ne 0 ]] && \
               echo "$OUTPUT" | grep -q "bb-freq-file"; then
                echo -e "${GREEN}  PASS: Missing BB frequency file correctly rejected${NC}"
            else
                echo -e "${RED}  FAIL: Expected hard failure for missing bb-freq-file${NC}"
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


echo ""
echo "=========================================="
echo "Test suite complete"
echo "=========================================="

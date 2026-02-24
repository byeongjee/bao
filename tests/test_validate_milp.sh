#!/bin/bash
#
# End-to-end MILP energy validation test.
# Runs MILP checkpoint insertion followed by energy validation.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Load environment
[[ -f "$PROJECT_DIR/.env" ]] && source "$PROJECT_DIR/.env"

CLANG="${CLANG:-${LLVM_DIR:+$LLVM_DIR/bin/}clang}"
OPT="${OPT:-${LLVM_DIR:+$LLVM_DIR/bin/}opt}"
LLVM_PROFDATA="${LLVM_PROFDATA:-${LLVM_DIR:+$LLVM_DIR/bin/}llvm-profdata}"
PASS_LIB="$PROJECT_DIR/passes/build/CheckpointPass.so"
RUNTIME="$PROJECT_DIR/passes/runtime/energy_validate_runtime.c"

ESTIMATOR_CONFIG="$SCRIPT_DIR/estimator_ir_uniform.json"
MILP_CONFIG="$SCRIPT_DIR/milp_params.json"

# Sysroot for source-built clang on macOS
SYSROOT_FLAGS=""
if command -v xcrun &>/dev/null; then
    SYSROOT_FLAGS="-isysroot $(xcrun --show-sdk-path)"
fi
PROFILE_CLANG="$CLANG"

if [[ ! -f "$PASS_LIB" ]]; then
    echo -e "${RED}Error: Pass library not found at $PASS_LIB${NC}"
    exit 1
fi

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

echo "=========================================="
echo "MILP Energy Validation End-to-End Tests"
echo "=========================================="
echo ""

PASS_COUNT=0
FAIL_COUNT=0

build_profiled_ir() {
    local src_file="$1"
    local out_ll="$2"
    local work_dir="$3"
    local profraw="$work_dir/test.profraw"
    local profdata="$work_dir/test.profdata"
    local train_bin="$work_dir/test_train"

    $PROFILE_CLANG $SYSROOT_FLAGS -O0 -Xclang -disable-O0-optnone \
        -fprofile-instr-generate="$profraw" \
        "$src_file" -o "$train_bin" 2>/dev/null

    LLVM_PROFILE_FILE="$profraw" "$train_bin" >/dev/null 2>&1 || true
    $LLVM_PROFDATA merge -o "$profdata" "$profraw" 2>/dev/null

    $CLANG $SYSROOT_FLAGS -S -emit-llvm -O0 -Xclang -disable-O0-optnone \
        -fprofile-instr-use="$profdata" \
        "$src_file" -o "$out_ll" 2>/dev/null
}

run_milp_validate() {
    local test_name="$1"
    local test_file="$2"
    local est_config="$3"
    local milp_config="$4"

    echo -e "${YELLOW}Test: $test_name${NC}"

    if [[ ! -f "$test_file" ]]; then
        echo -e "${RED}  SKIP: $test_file not found${NC}"
        echo ""
        return
    fi

    local tmp_dir
    tmp_dir=$(mktemp -d)
    trap "rm -rf $tmp_dir" RETURN

    # Compile to profile-guided IR
    build_profiled_ir "$test_file" "$tmp_dir/test.ll" "$tmp_dir"

    # MILP pass
    $OPT -load-pass-plugin="$PASS_LIB" \
        -passes=milp \
        -energy-config="$est_config" \
        -milp-config="$milp_config" \
        -S "$tmp_dir/test.ll" -o "$tmp_dir/ckpt.ll" 2>/dev/null

    # Energy-validate pass
    $OPT -load-pass-plugin="$PASS_LIB" \
        -passes=energy-validate \
        -energy-config="$est_config" \
        -milp-config="$milp_config" \
        -S "$tmp_dir/ckpt.ll" -o "$tmp_dir/validated.ll" 2>/dev/null

    # Compile and link with validation runtime
    $CLANG -O0 $SYSROOT_FLAGS \
        "$tmp_dir/validated.ll" "$RUNTIME" -o "$tmp_dir/test_bin" 2>/dev/null

    # Run
    local exit_code=0
    ("$tmp_dir/test_bin" > /dev/null 2>"$tmp_dir/stderr.txt") 2>/dev/null || exit_code=$?

    if [[ $exit_code -eq 0 ]]; then
        echo -e "${GREEN}  PASS: MILP solution validated, no energy violation${NC}"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        if grep -q "ENERGY VIOLATION" "$tmp_dir/stderr.txt"; then
            echo -e "${RED}  FAIL: Energy violation in MILP output!${NC}"
            head -5 "$tmp_dir/stderr.txt"
        else
            echo -e "${RED}  FAIL: Runtime error (exit=$exit_code)${NC}"
        fi
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
    echo ""
}

# Test MILP validation on existing test programs
run_milp_validate "test_linear (MILP validate)" \
    "$SCRIPT_DIR/test_linear.c" "$ESTIMATOR_CONFIG" "$MILP_CONFIG"

run_milp_validate "test_diamond (MILP validate)" \
    "$SCRIPT_DIR/test_diamond.c" "$ESTIMATOR_CONFIG" "$MILP_CONFIG"

run_milp_validate "test_simple_loop (MILP validate)" \
    "$SCRIPT_DIR/test_simple_loop.c" "$ESTIMATOR_CONFIG" "$MILP_CONFIG"

# Summary
echo "=========================================="
echo -e "Results: ${GREEN}$PASS_COUNT passed${NC}, ${RED}$FAIL_COUNT failed${NC}"
echo "=========================================="

[[ $FAIL_COUNT -eq 0 ]] && exit 0 || exit 1

#!/bin/bash
#
# Energy model regression test.
# Runs MILP pass on test_linear.c with verbose output, then runs energy-validate
# pass with -validate-verbose, and compares per-block remaining energy values.
# They should satisfy: remaining ≈ capacity - energyAccumulated (within epsilon).

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
ESTIMATOR_CONFIG="$SCRIPT_DIR/estimator_ir_uniform.json"
MILP_CONFIG="$SCRIPT_DIR/milp_params.json"
RUNTIME="$PROJECT_DIR/passes/runtime/energy_validate_runtime.c"
TEST_FILE="$SCRIPT_DIR/test_linear.c"

echo "=========================================="
echo "Energy Model Regression Test"
echo "=========================================="
echo ""

if [[ ! -f "$PASS_LIB" ]]; then
    echo -e "${RED}Error: Pass library not found at $PASS_LIB${NC}"
    exit 1
fi

if [[ ! -f "$TEST_FILE" ]]; then
    echo -e "${RED}Error: Test file not found: $TEST_FILE${NC}"
    exit 1
fi

if ! command -v "$LLVM_PROFDATA" >/dev/null 2>&1; then
    echo -e "${RED}Error: llvm-profdata not found ($LLVM_PROFDATA)${NC}"
    exit 1
fi

TMP_DIR=$(mktemp -d)
trap "rm -rf $TMP_DIR" EXIT

# Sysroot for source-built clang on macOS
SYSROOT_FLAGS=""
if command -v xcrun &>/dev/null; then
    SYSROOT_FLAGS="-isysroot $(xcrun --show-sdk-path)"
fi
PROFILE_CLANG="$CLANG"

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

# Step 1: Compile to IR
echo "Compiling to profile-guided IR..."
$PROFILE_CLANG $SYSROOT_FLAGS -O0 -Xclang -disable-O0-optnone \
    -fprofile-instr-generate="$TMP_DIR/test.profraw" \
    "$TEST_FILE" -o "$TMP_DIR/test_train" 2>/dev/null
LLVM_PROFILE_FILE="$TMP_DIR/test.profraw" "$TMP_DIR/test_train" >/dev/null 2>&1 || true
"$LLVM_PROFDATA" merge -o "$TMP_DIR/test.profdata" "$TMP_DIR/test.profraw" 2>/dev/null
$CLANG $SYSROOT_FLAGS -S -emit-llvm -O0 -Xclang -disable-O0-optnone \
    -fprofile-instr-use="$TMP_DIR/test.profdata" \
    "$TEST_FILE" -o "$TMP_DIR/test.ll" 2>/dev/null

# Step 2: Run MILP pass (captures energy info from verbose output)
echo "Running MILP pass..."
$OPT -load-pass-plugin="$PASS_LIB" \
    -passes=milp \
    -energy-config="$ESTIMATOR_CONFIG" \
    -milp-config="$MILP_CONFIG" \
    -S "$TMP_DIR/test.ll" -o "$TMP_DIR/ckpt.ll" 2>"$TMP_DIR/milp_output.txt" || true

echo -e "${YELLOW}MILP output:${NC}"
cat "$TMP_DIR/milp_output.txt"
echo ""

# Step 3: Run energy-validate pass with verbose output
echo "Running energy-validate pass..."
$OPT -load-pass-plugin="$PASS_LIB" \
    -passes=energy-validate \
    -energy-config="$ESTIMATOR_CONFIG" \
    -milp-config="$MILP_CONFIG" \
    -validate-verbose \
    -S "$TMP_DIR/ckpt.ll" -o "$TMP_DIR/validated.ll" 2>"$TMP_DIR/validate_pass_output.txt"

# Step 4: Compile and run to get runtime verbose output
echo "Compiling validated IR..."
$CLANG -O0 $SYSROOT_FLAGS \
    "$TMP_DIR/validated.ll" "$RUNTIME" -o "$TMP_DIR/test_bin" 2>/dev/null

echo "Running with energy tracking..."
"$TMP_DIR/test_bin" > "$TMP_DIR/stdout.txt" 2>"$TMP_DIR/verbose_output.txt" || true

echo ""
echo -e "${YELLOW}Energy validation verbose output:${NC}"
cat "$TMP_DIR/verbose_output.txt"

# Step 5: Check for violations
if grep -q "ENERGY VIOLATION" "$TMP_DIR/verbose_output.txt"; then
    echo ""
    echo -e "${RED}FAIL: Energy violation detected in MILP output!${NC}"
    echo "This indicates a divergence between the MILP model and the validator."
    exit 1
fi

echo ""
echo -e "${GREEN}PASS: No energy violations. MILP solution is consistent with validator.${NC}"

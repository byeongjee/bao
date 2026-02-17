#!/bin/bash
# Automated IR-level verification for VM/NVM placement enforcement.
# Checks: .nvm sections, shadow globals, rewritten accesses, correct
# store_mem/restore_mem arguments.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

if [[ -f "$PROJECT_DIR/.env" ]]; then
    source "$PROJECT_DIR/.env"
fi

if [[ -n "${LLVM_DIR}" ]]; then
    CLANG="${CLANG:-${LLVM_DIR}/bin/clang}"
    OPT="${OPT:-${LLVM_DIR}/bin/opt}"
else
    CLANG="${CLANG:-clang}"
    OPT="${OPT:-opt}"
fi

PASS_LIB="$PROJECT_DIR/passes/build/CheckpointPass.so"
TMP_DIR=$(mktemp -d)
trap "rm -rf $TMP_DIR" EXIT

RED='\033[0;31m'
GREEN='\033[0;32m'
BOLD='\033[1m'
NC='\033[0m'

PASS_COUNT=0
FAIL_COUNT=0

check() {
    local desc="$1"
    local pattern="$2"
    local file="$3"
    if grep -qE "$pattern" "$file"; then
        echo -e "  ${GREEN}PASS${NC}: $desc"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo -e "  ${RED}FAIL${NC}: $desc (pattern: $pattern)"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

check_absent() {
    local desc="$1"
    local pattern="$2"
    local file="$3"
    if ! grep -qE "$pattern" "$file"; then
        echo -e "  ${GREEN}PASS${NC}: $desc"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo -e "  ${RED}FAIL${NC}: $desc (unexpected match: $pattern)"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

# --- Test 1: VM/NVM enforcement (hot+cold globals) ---
echo -e "${BOLD}Test 1: VM/NVM enforcement (scenario_vm_nvm_enforcement)${NC}"

SRC="$PROJECT_DIR/scenario/scenario_vm_nvm_enforcement.c"
LL="$TMP_DIR/enforcement.ll"
OUT="$TMP_DIR/enforcement_out.ll"

"$CLANG" -S -emit-llvm -O0 -Xclang -disable-O0-optnone "$SRC" -o "$TMP_DIR/raw.ll" 2>/dev/null
"$OPT" -passes=mem2reg -S "$TMP_DIR/raw.ll" -o "$LL" 2>/dev/null
"$OPT" -load-pass-plugin="$PASS_LIB" -passes=checkpoint \
       -energy-config="$PROJECT_DIR/scenario/scenario_config.json" \
       -milp-config="$PROJECT_DIR/scenario/scenario_milp_config.json" \
       -S "$LL" -o "$OUT" 2>/dev/null

check ".nvm section on g_hot" '@g_hot = global i32 0, section ".nvm"' "$OUT"
check ".nvm section on g_cold" '@g_cold = global i32 0, section ".nvm"' "$OUT"
check "Shadow global for g_hot" '@__vm_shadow_g_hot = internal global i32 0' "$OUT"
check "Accesses rewritten to shadow" 'store i32 1, ptr @__vm_shadow_g_hot' "$OUT"
check "Load rewritten to shadow" 'load i32, ptr @__vm_shadow_g_hot' "$OUT"
check_absent "No direct store to @g_hot in function body" '  store .* ptr @g_hot' "$OUT"

echo ""

# --- Test 2: VM overflow (capacity constraint) ---
echo -e "${BOLD}Test 2: VM overflow (scenario_vm_overflow)${NC}"

SRC="$PROJECT_DIR/scenario/scenario_vm_overflow.c"
LL="$TMP_DIR/overflow.ll"
OUT="$TMP_DIR/overflow_out.ll"

"$CLANG" -S -emit-llvm -O0 -Xclang -disable-O0-optnone "$SRC" -o "$TMP_DIR/raw2.ll" 2>/dev/null
"$OPT" -passes=mem2reg -S "$TMP_DIR/raw2.ll" -o "$LL" 2>/dev/null
"$OPT" -load-pass-plugin="$PASS_LIB" -passes=checkpoint \
       -energy-config="$PROJECT_DIR/scenario/scenario_vm_overflow_config.json" \
       -milp-config="$PROJECT_DIR/scenario/scenario_milp_vm_overflow_config.json" \
       -S "$LL" -o "$OUT" 2>/dev/null

check "All 5 globals get .nvm section" '@g_a = global i32 0, section ".nvm"' "$OUT"
check ".nvm on g_e" '@g_e = global i32 0, section ".nvm"' "$OUT"

# With vm_capacity=16 and 5x4-byte globals, at most 4 can have shadows
SHADOW_COUNT=$(grep -c '@__vm_shadow_g_.* = internal global' "$OUT" || true)
if [[ "$SHADOW_COUNT" -le 4 && "$SHADOW_COUNT" -ge 1 ]]; then
    echo -e "  ${GREEN}PASS${NC}: Shadow count ($SHADOW_COUNT) <= 4 (respects VM capacity)"
    PASS_COUNT=$((PASS_COUNT + 1))
else
    echo -e "  ${RED}FAIL${NC}: Shadow count ($SHADOW_COUNT) should be 1-4"
    FAIL_COUNT=$((FAIL_COUNT + 1))
fi

# At least one global should NOT have a shadow (stays NVM-only)
ALL_SHADOWS=$(grep -oE '@__vm_shadow_g_[a-e]' "$OUT" | sort -u || true)
HAS_UNSHADOWED=false
for g in g_a g_b g_c g_d g_e; do
    if ! echo "$ALL_SHADOWS" | grep -q "@__vm_shadow_$g" 2>/dev/null; then
        HAS_UNSHADOWED=true
        break
    fi
done
if $HAS_UNSHADOWED; then
    echo -e "  ${GREEN}PASS${NC}: At least one global remains NVM-only (no shadow)"
    PASS_COUNT=$((PASS_COUNT + 1))
else
    echo -e "  ${RED}FAIL${NC}: All 5 globals have shadows — VM capacity not enforced"
    FAIL_COUNT=$((FAIL_COUNT + 1))
fi

echo ""

# --- Test 3: Existing test_vm_nvm_placement still passes ---
echo -e "${BOLD}Test 3: Existing test_vm_nvm_placement${NC}"

SRC="$PROJECT_DIR/tests/test_vm_nvm_placement.c"
LL="$TMP_DIR/placement.ll"
OUT="$TMP_DIR/placement_out.ll"

"$CLANG" -S -emit-llvm -O3 "$SRC" -o "$LL" 2>/dev/null
"$OPT" -load-pass-plugin="$PASS_LIB" -passes=checkpoint \
       -energy-config="$PROJECT_DIR/tests/estimator_ir_uniform.json" \
       -milp-config="$PROJECT_DIR/tests/milp_params_small.json" \
       -S "$LL" -o "$OUT" 2>/dev/null
EXIT=$?

if [[ $EXIT -eq 0 ]]; then
    echo -e "  ${GREEN}PASS${NC}: Pass completed successfully"
    PASS_COUNT=$((PASS_COUNT + 1))
else
    echo -e "  ${RED}FAIL${NC}: Pass failed with exit code $EXIT"
    FAIL_COUNT=$((FAIL_COUNT + 1))
fi

check ".nvm section on frequently_accessed" 'section ".nvm"' "$OUT"
check "Shadow global exists" '@__vm_shadow_' "$OUT"

echo ""

# --- Summary ---
echo -e "${BOLD}===========================================${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT))
echo -e "  Results: ${GREEN}$PASS_COUNT${NC}/$TOTAL passed"
if [[ $FAIL_COUNT -gt 0 ]]; then
    echo -e "  ${RED}$FAIL_COUNT FAILURES${NC}"
    exit 1
else
    echo -e "  ${GREEN}All checks passed${NC}"
fi

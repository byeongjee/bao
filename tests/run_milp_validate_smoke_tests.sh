#!/bin/bash

# Smoke tests for phase-2 milp + milp-validate pipeline.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
TMP_DIR="$PROJECT_DIR/tmp"

if [[ -f "$PROJECT_DIR/.env" ]]; then
    source "$PROJECT_DIR/.env"
fi

if [[ -n "${LLVM_DIR:-}" ]]; then
    CLANG="${CLANG:-${LLVM_DIR}/bin/clang}"
    OPT="${OPT:-${LLVM_DIR}/bin/opt}"
else
    CLANG="${CLANG:-clang}"
    OPT="${OPT:-opt}"
fi

PASS_LIB="$PROJECT_DIR/passes/build/CheckpointPass.so"
ENERGY_CONFIG="$SCRIPT_DIR/simple_config.json"
MILP_CONFIG="$PROJECT_DIR/benchmarks/sample_milp_config.json"

mkdir -p "$TMP_DIR"

if [[ ! -f "$PASS_LIB" ]]; then
    echo "Error: pass library not found at $PASS_LIB"
    exit 1
fi
if [[ ! -f "$ENERGY_CONFIG" ]]; then
    echo "Error: missing energy config $ENERGY_CONFIG"
    exit 1
fi
if [[ ! -f "$MILP_CONFIG" ]]; then
    echo "Error: missing MILP config $MILP_CONFIG"
    exit 1
fi

echo "== milp-validate smoke test =="

INPUT_C="$SCRIPT_DIR/test_linear.c"
INPUT_LL="$TMP_DIR/milp_validate_smoke.ll"
OUTPUT_LL="$TMP_DIR/milp_validate_smoke_out.ll"

"$CLANG" -S -emit-llvm -O0 -Xclang -disable-O0-optnone "$INPUT_C" -o "$INPUT_LL"

PASS_OUTPUT=$("$OPT" -load-pass-plugin="$PASS_LIB" \
    -passes=milp,milp-validate \
    -energy-config="$ENERGY_CONFIG" \
    -milp-config="$MILP_CONFIG" \
    "$INPUT_LL" -S -o "$OUTPUT_LL" 2>&1)

echo "$PASS_OUTPUT"

if ! echo "$PASS_OUTPUT" | grep -q "milp-validate PASS"; then
    echo "FAIL: expected milp-validate PASS output"
    exit 1
fi

TINY_BUDGET_CONFIG="$TMP_DIR/milp_tiny_budget_config.json"
cat > "$TINY_BUDGET_CONFIG" <<'JSON'
{
  "version": "1.0",
  "milp_parameters": {
    "energy_budget_nJ": 1.0,
    "region_prologue_overhead_nJ": 0.0,
    "region_epilogue_overhead_nJ": 0.0,
    "vm_capacity_bytes": 2048,
    "save_register_to_nvm_nJ": 800.0,
    "restore_register_from_nvm_nJ": 700.0,
    "save_object_to_nvm_per_byte_nJ": 20.0,
    "restore_object_from_nvm_per_byte_nJ": 20.0,
    "nvm_load_penalty_per_byte_nJ": 5.0,
    "nvm_store_penalty_per_byte_nJ": 10.0,
    "boundary_reboot_probability": 1.0,
    "default_loop_bound": 2,
    "solver_time_limit_sec": 30,
    "solver_mip_gap": 0.01,
    "forbidden_boundary_blocks": []
  }
}
JSON

FAIL_OUTPUT=$("$OPT" -load-pass-plugin="$PASS_LIB" \
    -passes=milp,milp-validate \
    -milp-validate-strict=false \
    -energy-config="$ENERGY_CONFIG" \
    -milp-config="$TINY_BUDGET_CONFIG" \
    "$INPUT_LL" -S -o /dev/null 2>&1 || true)

echo "$FAIL_OUTPUT"

if ! echo "$FAIL_OUTPUT" | grep -Eq "milp-validate failed|milp boundary planning failed|milp-validate missing region metadata"; then
    echo "FAIL: expected milp pipeline failure message"
    exit 1
fi

echo "PASS: milp-validate smoke tests completed"

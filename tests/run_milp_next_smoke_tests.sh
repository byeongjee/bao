#!/bin/bash

# Smoke tests for phase-1 milp-next pass wiring.

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

echo "== milp-next smoke test =="

INPUT_C="$SCRIPT_DIR/test_linear.c"
INPUT_LL="$TMP_DIR/milp_next_smoke.ll"
OUTPUT_LL="$TMP_DIR/milp_next_smoke_out.ll"

"$CLANG" -S -emit-llvm -O0 -Xclang -disable-O0-optnone "$INPUT_C" -o "$INPUT_LL"

PASS_OUTPUT=$("$OPT" -load-pass-plugin="$PASS_LIB" \
    -passes=milp-next \
    -energy-config="$ENERGY_CONFIG" \
    -milp-config="$MILP_CONFIG" \
    "$INPUT_LL" -S -o "$OUTPUT_LL" 2>&1)

echo "$PASS_OUTPUT"

if ! echo "$PASS_OUTPUT" | grep -q "milp bring-up (phase-2)"; then
    echo "FAIL: expected milp bring-up diagnostics"
    exit 1
fi

BROKEN_CONFIG="$TMP_DIR/milp_next_broken_config.json"
cat > "$BROKEN_CONFIG" <<'JSON'
{
  "version": "1.0",
  "milp_parameters": {
    "energy_budget_nJ": 500000.0,
    "region_prologue_overhead_nJ": 2000.0,
    "region_epilogue_overhead_nJ": 2000.0,
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
    "forbidden_boundary_blocks": []
  }
}
JSON

BROKEN_OUTPUT=$("$OPT" -load-pass-plugin="$PASS_LIB" \
    -passes=milp-next \
    -energy-config="$ENERGY_CONFIG" \
    -milp-config="$BROKEN_CONFIG" \
    "$INPUT_LL" -S -o /dev/null 2>&1 || true)

echo "$BROKEN_OUTPUT"

if ! echo "$BROKEN_OUTPUT" | grep -q "Missing required milp parameter 'solver_mip_gap'"; then
    echo "FAIL: expected missing-field error for invalid milp config"
    exit 1
fi

echo "PASS: milp-next smoke tests completed"

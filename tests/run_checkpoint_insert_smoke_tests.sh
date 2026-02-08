#!/bin/bash

# Smoke tests for algorithm-dispatch checkpoint insertion interface.

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
MILP_ENERGY_CONFIG="$SCRIPT_DIR/simple_config.json"
ROCKCLIMB_ENERGY_CONFIG="$SCRIPT_DIR/rockclimb_config.json"
MILP_CONFIG="$PROJECT_DIR/benchmarks/sample_milp_config.json"

mkdir -p "$TMP_DIR"

if [[ ! -f "$PASS_LIB" ]]; then
    echo "Error: pass library not found at $PASS_LIB"
    exit 1
fi
if [[ ! -f "$MILP_ENERGY_CONFIG" ]]; then
    echo "Error: missing energy config $MILP_ENERGY_CONFIG"
    exit 1
fi
if [[ ! -f "$ROCKCLIMB_ENERGY_CONFIG" ]]; then
    echo "Error: missing rockclimb config $ROCKCLIMB_ENERGY_CONFIG"
    exit 1
fi
if [[ ! -f "$MILP_CONFIG" ]]; then
    echo "Error: missing MILP config $MILP_CONFIG"
    exit 1
fi

echo "== checkpoint-insert interface smoke test =="

INPUT_C="$SCRIPT_DIR/test_linear.c"
INPUT_LL="$TMP_DIR/checkpoint_insert_smoke.ll"
OUTPUT_LL="$TMP_DIR/checkpoint_insert_smoke_out.ll"

"$CLANG" -S -emit-llvm -O0 -Xclang -disable-O0-optnone "$INPUT_C" -o "$INPUT_LL"

MILP_OUTPUT=$("$OPT" -load-pass-plugin="$PASS_LIB" \
    -passes=checkpoint-insert \
    -checkpoint-algorithm=milp \
    -energy-config="$MILP_ENERGY_CONFIG" \
    -milp-config="$MILP_CONFIG" \
    "$INPUT_LL" -S -o "$OUTPUT_LL" 2>&1)
echo "$MILP_OUTPUT"

if ! echo "$MILP_OUTPUT" | grep -q "checkpoint-insert: algorithm='milp'"; then
    echo "FAIL: expected checkpoint-insert milp dispatcher output"
    exit 1
fi
if ! echo "$MILP_OUTPUT" | grep -q "milp bring-up (phase-2)"; then
    echo "FAIL: expected milp pass output through dispatcher"
    exit 1
fi

ROCKCLIMB_OUTPUT=$("$OPT" -load-pass-plugin="$PASS_LIB" \
    -passes=checkpoint-insert \
    -checkpoint-algorithm=rockclimb \
    -energy-config="$ROCKCLIMB_ENERGY_CONFIG" \
    "$INPUT_LL" -S -o "$OUTPUT_LL" 2>&1)
echo "$ROCKCLIMB_OUTPUT"

if ! echo "$ROCKCLIMB_OUTPUT" | grep -q "checkpoint-insert: algorithm='rockclimb'"; then
    echo "FAIL: expected checkpoint-insert rockclimb dispatcher output"
    exit 1
fi
if ! echo "$ROCKCLIMB_OUTPUT" | grep -q "RockClimb Pass"; then
    echo "FAIL: expected rockclimb output through dispatcher"
    exit 1
fi

INVALID_OUTPUT=$("$OPT" -load-pass-plugin="$PASS_LIB" \
    -passes=checkpoint-insert \
    -checkpoint-algorithm=unknown \
    -energy-config="$MILP_ENERGY_CONFIG" \
    "$INPUT_LL" -S -o /dev/null 2>&1 || true)
echo "$INVALID_OUTPUT"

if ! echo "$INVALID_OUTPUT" | grep -q "unsupported checkpoint algorithm"; then
    echo "FAIL: expected invalid algorithm error"
    exit 1
fi

echo "PASS: checkpoint-insert interface smoke tests completed"

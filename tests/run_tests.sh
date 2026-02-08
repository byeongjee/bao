#!/bin/bash

# Top-level regression runner for checkpoint insertion algorithms.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

TEST_SCRIPTS=(
    "run_checkpoint_insert_smoke_tests.sh"
    "run_milp_next_smoke_tests.sh"
    "run_milp_validate_smoke_tests.sh"
    "run_rockclimb_tests.sh"
    "run_memory_ckpt_tests.sh"
)

echo "=========================================="
echo "Checkpoint Insertion Test Suite"
echo "=========================================="

for script in "${TEST_SCRIPTS[@]}"; do
    echo ""
    echo ">>> Running ${script}"
    "${SCRIPT_DIR}/${script}"
done

echo ""
echo "=========================================="
echo "All checkpoint insertion tests completed"
echo "=========================================="

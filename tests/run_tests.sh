#!/bin/bash

# Test runner: the pytest suite, in the Docker image
# Usage: ./run_tests.sh [--milp] [--rockclimb]
#   No flags    = run all tests
#   --milp      = run only MILP tests
#   --rockclimb = run only RockClimb tests

set -e

# The compilation toolchain is in the Docker image; outside it, run there.
if [[ "${CKPT_IN_DOCKER:-}" != 1 ]]; then
    exec docker run --rm -v "$GRB_LICENSE_FILE:/licenses/gurobi.lic:ro" bao tests/run_tests.sh "$@"
fi

MARKS=""
for arg in "$@"; do
    case "$arg" in
        --milp) MARKS="${MARKS:+$MARKS or }milp" ;;
        --rockclimb) MARKS="${MARKS:+$MARKS or }rockclimb" ;;
        *) echo "Unknown option: $arg"; echo "Usage: $0 [--milp] [--rockclimb]"; exit 1 ;;
    esac
done

cd "$(dirname "${BASH_SOURCE[0]}")/.."
if [[ -z "$MARKS" ]]; then
    exec uv run pytest tests/
fi
exec uv run pytest tests/ -m "$MARKS"

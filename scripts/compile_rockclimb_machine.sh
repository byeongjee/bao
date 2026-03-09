#!/usr/bin/env bash
#
# Compile C source with machine-level RockClimb checkpoint insertion.
# Uses post-regalloc MachineFunctionPass via MIR pipeline.
#
# Usage:
#   compile_rockclimb_machine.sh -e <energy_config> -c <rockclimb_config> [options] <input.c>
#
# Options:
#   -e <config>          Assembly energy config JSON (required)
#   -c <config>          RockClimb config JSON (required)
#   -o <output>          Output base name (default: build/<input>)
#   -Oc <level>          Clang optimization level (default: 2)
#   --verbose            Show detailed pass output
#   -h, --help           Show this help message
#
# Pipeline:
#   C → clang → .ll → llc -stop-after=virtregrewriter → .mir
#   → llc -load=RockClimbMachinePass.so -run-pass=rockclimb-machine → .mir
#   → llc -start-after=virtregrewriter → .s
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib/common.sh"

# Pass plugin
MACHINE_PASS_LIB="$PROJECT_DIR/passes/build/rockclimb-backend/RockClimbMachinePass.so"

# Defaults
ESTIMATOR_CONFIG=""
ROCKCLIMB_CONFIG=""
OUTPUT=""
CLANG_OPT_LEVEL="2"
VERBOSE="false"
INPUT=""

usage() { sed -n '2,18p' "$0" | sed 's/^# \?//'; exit 0; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        -e|--energy-config) ESTIMATOR_CONFIG="$2"; shift 2 ;;
        -c|--rockclimb-config) ROCKCLIMB_CONFIG="$2"; shift 2 ;;
        -o) OUTPUT="$2"; shift 2 ;;
        -Oc) CLANG_OPT_LEVEL="$2"; shift 2 ;;
        --verbose) VERBOSE="true"; shift ;;
        -h|--help) usage ;;
        -*) echo "Unknown option: $1" >&2; exit 1 ;;
        *) INPUT="$1"; shift ;;
    esac
done

[[ -z "$INPUT" ]] && { echo "Error: No input file" >&2; exit 1; }
[[ -z "$ESTIMATOR_CONFIG" ]] && { echo "Error: -e <energy_config> required" >&2; exit 1; }
[[ -z "$ROCKCLIMB_CONFIG" ]] && { echo "Error: -c <rockclimb_config> required" >&2; exit 1; }
[[ ! -f "$MACHINE_PASS_LIB" ]] && { echo "Error: $MACHINE_PASS_LIB not found. Build with: cd passes/build && make RockClimbMachinePass" >&2; exit 1; }

BASENAME="$(basename "${INPUT%.*}")"
OUTPUT="${OUTPUT:-build/$BASENAME}"
mkdir -p "$(dirname "$OUTPUT")"

# Step 1: C → LLVM IR
echo "=== Step 1: C → LLVM IR (clang -O${CLANG_OPT_LEVEL}) ==="
"$CLANG" -S -emit-llvm -O"$CLANG_OPT_LEVEL" --target=msp430 \
    "$INPUT" -o "${OUTPUT}.ll"

# Step 2: LLVM IR → MIR (stop after register allocation)
echo "=== Step 2: IR → MIR (stop-after=virtregrewriter) ==="
"$LLC" -march=msp430 -stop-after=virtregrewriter \
    "${OUTPUT}.ll" -o "${OUTPUT}.mir"

# Step 3: Run RockClimb machine pass on MIR
echo "=== Step 3: RockClimb Machine Pass ==="
PASS_STDERR=""
if [[ "$VERBOSE" == "true" ]]; then
    "$LLC" -march=msp430 \
        -load="$MACHINE_PASS_LIB" \
        -run-pass=rockclimb-machine \
        -rockclimb-machine-config="$ROCKCLIMB_CONFIG" \
        -rockclimb-machine-energy-config="$ESTIMATOR_CONFIG" \
        "${OUTPUT}.mir" -o "${OUTPUT}.instrumented.mir"
else
    "$LLC" -march=msp430 \
        -load="$MACHINE_PASS_LIB" \
        -run-pass=rockclimb-machine \
        -rockclimb-machine-config="$ROCKCLIMB_CONFIG" \
        -rockclimb-machine-energy-config="$ESTIMATOR_CONFIG" \
        "${OUTPUT}.mir" -o "${OUTPUT}.instrumented.mir" 2>/dev/null
fi

# Step 4: Resume compilation: instrumented MIR → assembly
echo "=== Step 4: MIR → Assembly (start-after=virtregrewriter) ==="
"$LLC" -march=msp430 -start-after=virtregrewriter \
    "${OUTPUT}.instrumented.mir" -o "${OUTPUT}.s"

echo "=== Done ==="
echo "  LLVM IR:           ${OUTPUT}.ll"
echo "  MIR (pre-pass):    ${OUTPUT}.mir"
echo "  MIR (post-pass):   ${OUTPUT}.instrumented.mir"
echo "  Assembly:          ${OUTPUT}.s"

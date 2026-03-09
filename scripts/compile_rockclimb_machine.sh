#!/usr/bin/env bash
#
# Compile C source with machine-level RockClimb checkpoint insertion.
# Uses post-regalloc MachineFunctionPass via MIR pipeline.
#
# Usage:
#   compile_rockclimb_machine.sh -e <energy_config> -c <rockclimb_config> [options] <input.c>
#
# Options:
#   -e <config>          Assembly energy config/params JSON (required)
#   -c <config>          RockClimb config JSON (required)
#   -o <output>          Output base name (default: build/<input>)
#   -Oc <level>          Clang optimization level (default: 2)
#   --no-precomputed-energy  Skip bb-energy-analyzer, use MIR-level estimation
#   --verbose            Show detailed pass output
#   --link               Assemble + link to produce .elf
#   --mock-counter       Link with mock counter runtime (implies --link)
#   --linker <script>    Linker script (default: rockclimb_msp430fr5994.ld)
#   -h, --help           Show this help message
#
# Default pipeline (pre-computed energy):
#   C → clang → .ll → opt (assign-bb-debuginfo) → .ll
#   .ll → llc → .o → bb-energy-analyzer → energy.json
#   .ll → llc -stop-after=virtregrewriter → .mir
#   → llc -load=RockClimbMachinePass.so -run-pass=rockclimb-machine → .mir
#   → llc -start-after=virtregrewriter → .s
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib/common.sh"

# Pass plugin
MACHINE_PASS_LIB="$PROJECT_DIR/passes/build/rockclimb-backend/RockClimbMachinePass.so"

# BB debug info pass plugin (for assign-bb-debuginfo)
BB_DEBUGINFO_LIB="$PROJECT_DIR/passes/build/bb-debuginfo/BBDebugInfoPass.so"

# bb-energy-analyzer
BB_ANALYZER="$PROJECT_DIR/passes/build/bb-energy-analyzer/bb-energy-analyzer"

# Defaults
ESTIMATOR_CONFIG=""
ROCKCLIMB_CONFIG=""
OUTPUT=""
CLANG_OPT_LEVEL="2"
PRECOMPUTED_ENERGY="true"
VERBOSE="false"
LINK="false"
MOCK_COUNTER="false"
LINKER_SCRIPT=""
INPUT=""

usage() { sed -n '2,29p' "$0" | sed 's/^# \?//'; exit 0; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        -e|--energy-config) ESTIMATOR_CONFIG="$2"; shift 2 ;;
        -c|--rockclimb-config) ROCKCLIMB_CONFIG="$2"; shift 2 ;;
        -o) OUTPUT="$2"; shift 2 ;;
        -Oc) CLANG_OPT_LEVEL="$2"; shift 2 ;;
        --no-precomputed-energy) PRECOMPUTED_ENERGY="false"; shift ;;
        --verbose) VERBOSE="true"; shift ;;
        --link) LINK="true"; shift ;;
        --mock-counter) MOCK_COUNTER="true"; LINK="true"; shift ;;
        --linker) LINKER_SCRIPT="$2"; shift 2 ;;
        -h|--help) usage ;;
        -*) echo "Unknown option: $1" >&2; exit 1 ;;
        *) INPUT="$1"; shift ;;
    esac
done

[[ -z "$INPUT" ]] && { echo "Error: No input file" >&2; exit 1; }
[[ -z "$ESTIMATOR_CONFIG" ]] && { echo "Error: -e <energy_config> required" >&2; exit 1; }
[[ -z "$ROCKCLIMB_CONFIG" ]] && { echo "Error: -c <rockclimb_config> required" >&2; exit 1; }
[[ ! -f "$MACHINE_PASS_LIB" ]] && { echo "Error: $MACHINE_PASS_LIB not found. Build with: cd passes/build && make RockClimbMachinePass" >&2; exit 1; }

if [[ "$PRECOMPUTED_ENERGY" == "true" ]]; then
    [[ ! -f "$BB_DEBUGINFO_LIB" ]] && { echo "Error: $BB_DEBUGINFO_LIB not found (needed for assign-bb-debuginfo). Build with: cd passes/build && make BBDebugInfoPass" >&2; exit 1; }
    [[ ! -f "$BB_ANALYZER" ]] && { echo "Error: $BB_ANALYZER not found. Build with: cd passes/build && make bb-energy-analyzer" >&2; exit 1; }
fi

BASENAME="$(basename "${INPUT%.*}")"
OUTPUT="${OUTPUT:-build/$BASENAME}"
mkdir -p "$(dirname "$OUTPUT")"

# Step 1: C → LLVM IR
echo "=== Step 1: C → LLVM IR (clang -O${CLANG_OPT_LEVEL}) ==="
"$CLANG" -S -emit-llvm -O"$CLANG_OPT_LEVEL" --target=msp430 \
    "$INPUT" -o "${OUTPUT}.ll"

if [[ "$PRECOMPUTED_ENERGY" == "true" ]]; then
    # Step 2: Assign BB debug info for DWARF-based energy analysis
    echo "=== Step 2: Assign BB debug info ==="
    "$OPT" -load-pass-plugin="$BB_DEBUGINFO_LIB" \
        -passes=assign-bb-debuginfo \
        -bb-mapping="${OUTPUT}.bb_mapping.json" \
        -S "${OUTPUT}.ll" -o "${OUTPUT}.bbinfo.ll"

    # Step 3a: IR → MIR (stop after register allocation) — uses bbinfo IR
    echo "=== Step 3a: IR → MIR (stop-after=virtregrewriter) ==="
    "$LLC" -march=msp430 -stop-after=virtregrewriter \
        "${OUTPUT}.bbinfo.ll" -o "${OUTPUT}.mir"

    # Step 3b: IR → object file (full pipeline) — for bb-energy-analyzer
    echo "=== Step 3b: IR → object file (for energy analysis) ==="
    "$LLC" -march=msp430 -filetype=obj \
        "${OUTPUT}.bbinfo.ll" -o "${OUTPUT}.energy.o"

    # Step 3c: bb-energy-analyzer: compute per-BB energy from real assembly
    echo "=== Step 3c: bb-energy-analyzer → per-BB energy JSON ==="
    "$BB_ANALYZER" \
        --energy-params "$ESTIMATOR_CONFIG" \
        --bb-mapping "${OUTPUT}.bb_mapping.json" \
        "${OUTPUT}.energy.o" > "${OUTPUT}.bb_energy.json"

    # Step 4: Run RockClimb machine pass on MIR with pre-computed energy
    echo "=== Step 4: RockClimb Machine Pass (pre-computed energy) ==="
    if [[ "$VERBOSE" == "true" ]]; then
        "$LLC" -march=msp430 \
            -load="$MACHINE_PASS_LIB" \
            -run-pass=rockclimb-machine \
            -rockclimb-machine-config="$ROCKCLIMB_CONFIG" \
            -rockclimb-machine-energy-data="${OUTPUT}.bb_energy.json" \
            "${OUTPUT}.mir" -o "${OUTPUT}.instrumented.mir"
    else
        "$LLC" -march=msp430 \
            -load="$MACHINE_PASS_LIB" \
            -run-pass=rockclimb-machine \
            -rockclimb-machine-config="$ROCKCLIMB_CONFIG" \
            -rockclimb-machine-energy-data="${OUTPUT}.bb_energy.json" \
            "${OUTPUT}.mir" -o "${OUTPUT}.instrumented.mir" 2>/dev/null
    fi
else
    # Fallback: MIR-level energy estimation (no bb-energy-analyzer)

    # Step 2: LLVM IR → MIR (stop after register allocation)
    echo "=== Step 2: IR → MIR (stop-after=virtregrewriter) ==="
    "$LLC" -march=msp430 -stop-after=virtregrewriter \
        "${OUTPUT}.ll" -o "${OUTPUT}.mir"

    # Step 3: Run RockClimb machine pass on MIR
    echo "=== Step 3: RockClimb Machine Pass (MIR-level estimation) ==="
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
fi

# Step 5: Resume compilation: instrumented MIR → assembly
echo "=== Step 5: MIR → Assembly (start-after=virtregrewriter) ==="
"$LLC" -march=msp430 -start-after=virtregrewriter \
    "${OUTPUT}.instrumented.mir" -o "${OUTPUT}.s"

# Step 6 (optional): Assemble + link
if [[ "$LINK" == "true" ]]; then
    echo "=== Step 6: Assemble + Link ==="

    # Assemble to object
    $GCC -mmcu=$DEVICE -msmall -c "${OUTPUT}.s" -o "${OUTPUT}.o"

    # Determine linker script
    [[ -z "$LINKER_SCRIPT" ]] && LINKER_SCRIPT="$PROJECT_DIR/passes/runtime/rockclimb_msp430fr5994.ld"

    if [[ "$MOCK_COUNTER" == "true" ]]; then
        MOCK_COUNTER_SRC="$PROJECT_DIR/passes/runtime/rockclimb_machine_mock_ckpt_counter.c"
        [[ ! -f "$MOCK_COUNTER_SRC" ]] && { echo "Error: $MOCK_COUNTER_SRC not found" >&2; exit 1; }

        $GCC -mmcu=$DEVICE -msmall -O2 \
            -I"$MSP430GCC_SUPPORT_PATH/include" \
            -I"$PROJECT_DIR/passes/runtime" \
            -c "$MOCK_COUNTER_SRC" -o "${OUTPUT}.mock.o"

        $GCC -mmcu=$DEVICE -msmall \
            -L"$MSP430GCC_SUPPORT_PATH/include" \
            -T "$LINKER_SCRIPT" \
            "${OUTPUT}.o" "${OUTPUT}.mock.o" -o "${OUTPUT}.elf"
    else
        $GCC -mmcu=$DEVICE -msmall \
            -L"$MSP430GCC_SUPPORT_PATH/include" \
            -T "$LINKER_SCRIPT" \
            "${OUTPUT}.o" -o "${OUTPUT}.elf"
    fi

    $SIZE "${OUTPUT}.elf"
fi

echo "=== Done ==="
echo "  LLVM IR:           ${OUTPUT}.ll"
echo "  MIR (pre-pass):    ${OUTPUT}.mir"
echo "  MIR (post-pass):   ${OUTPUT}.instrumented.mir"
echo "  Assembly:          ${OUTPUT}.s"
[[ "$LINK" == "true" ]] && echo "  ELF:               ${OUTPUT}.elf"
exit 0

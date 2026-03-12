#!/usr/bin/env bash
#
# Compile C source with MILP checkpoint insertion.
#
# Usage:
#   compile_milp.sh -e <energy_config> -m <milp_config> [options] <input.c>
#
# Options:
#   -e <config>          Energy estimator config JSON (required)
#   -m <config>          MILP config JSON (required)
#   -o <output>          Output base name (default: build/<input>)
#   -O <level>           LLC optimization level (default: 2)
#   -Oc <level>          Clang optimization level (default: 2)
#   -I <dir>             Add include directory (can be repeated)
#   --link               Assemble + link for MSP430
#   --halt-mode <mode>   Halt mode: debug (default), bor, lpm4
#   --debug-counters     Link debug counter runtime (UART + NVM counters)
#   --verbose            Show detailed pass output
#   --debug              Enable DEBUG output
#   --add-debug-markers  Insert debug counter markers in IR
#   --estimator-mode <m> Energy estimator: assembly (default), ir
#   --ir-energy          Shorthand for --estimator-mode ir
#   -h, --help           Show this help message
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib/common.sh"

# Defaults
ESTIMATOR_CONFIG=""
MILP_CONFIG=""
OUTPUT=""
OPT_LEVEL="2"
CLANG_OPT_LEVEL="2"
EXTRA_INCLUDES=""
VERBOSE="false"
DEBUG_MODE="false"
ADD_DEBUG_MARKERS="false"
LINK="false"
HALT_MODE="debug"
DEBUG_COUNTERS="false"
ESTIMATOR_MODE="assembly"
INPUT=""

usage() { sed -n '2,24p' "$0" | sed 's/^# \?//'; exit 0; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        -e|--energy-config) ESTIMATOR_CONFIG="$2"; shift 2 ;;
        -m|--milp-config) MILP_CONFIG="$2"; shift 2 ;;
        -o) OUTPUT="$2"; shift 2 ;;
        -O) OPT_LEVEL="$2"; shift 2 ;;
        -Oc) CLANG_OPT_LEVEL="$2"; shift 2 ;;
        -I) EXTRA_INCLUDES="$EXTRA_INCLUDES -I$2"; shift 2 ;;
        --link) LINK="true"; shift ;;
        --halt-mode) HALT_MODE="$2"; shift 2 ;;
        --debug-counters) DEBUG_COUNTERS="true"; shift ;;
        --verbose) VERBOSE="true"; shift ;;
        --debug) DEBUG_MODE="true"; shift ;;
        --add-debug-markers) ADD_DEBUG_MARKERS="true"; shift ;;
        --estimator-mode) ESTIMATOR_MODE="$2"; shift 2 ;;
        --ir-energy) ESTIMATOR_MODE="ir"; shift ;;
        -h|--help) usage ;;
        -*) error "Unknown option: $1" ;;
        *) INPUT="$1"; shift ;;
    esac
done

# bor/lpm4 halt modes and debug-counters imply --link
[[ "$HALT_MODE" == "bor" || "$HALT_MODE" == "lpm4" ]] && LINK="true"
[[ "$DEBUG_COUNTERS" == "true" ]] && LINK="true"

# Validate
[[ -z "$INPUT" ]] && error "No input file specified"
[[ ! -f "$INPUT" ]] && error "File not found: $INPUT"
[[ -z "$ESTIMATOR_CONFIG" ]] && error "Energy config required: use -e <config.json>"
[[ ! -f "$ESTIMATOR_CONFIG" ]] && error "Estimator config not found: $ESTIMATOR_CONFIG"
[[ -z "$MILP_CONFIG" ]] && error "MILP config required: use -m <config.json>"
[[ ! -f "$MILP_CONFIG" ]] && error "MILP config not found: $MILP_CONFIG"
[[ ! -f "$PASS_LIB" ]] && error "Pass not found: $PASS_LIB"
if [[ "$ESTIMATOR_MODE" != "assembly" && "$ESTIMATOR_MODE" != "ir" ]]; then
    error "Unknown estimator mode: $ESTIMATOR_MODE (use: assembly, ir)"
fi
if [[ "$ESTIMATOR_MODE" == "assembly" ]]; then
    [[ ! -f "$BB_DEBUGINFO_LIB" ]] && error "BBDebugInfoPass.so not found (needed for assembly energy)"
    [[ ! -f "$BB_ANALYZER" ]] && error "bb-energy-analyzer not found (needed for assembly energy)"
fi

# Output name
BUILD_DIR="$PROJECT_DIR/build"
[[ -z "$OUTPUT" ]] && OUTPUT="$BUILD_DIR/$(basename "$INPUT" .c)"
mkdir -p "$(dirname "$OUTPUT")"

TMP_DIR=$(mktemp -d)
trap "rm -rf $TMP_DIR" EXIT

# Build compile_to_ir args — compile at -O0 to preserve loops for tripcount
IR_ARGS=("$INPUT" "$TMP_DIR/input.ll" --clang-opt-level 0
    -I "$PROJECT_DIR/passes/runtime")
[[ "$DEBUG_MODE" == "true" ]] && IR_ARGS+=(--debug)
[[ "$DEBUG_COUNTERS" == "true" ]] && IR_ARGS+=(-D DEBUG_COUNTERS)
for word in $EXTRA_INCLUDES; do
    if [[ "$word" == -I* ]]; then
        IR_ARGS+=(-I "${word#-I}")
    fi
done

info "Compiling with MILP..."
compile_to_ir "${IR_ARGS[@]}"

# TripCount annotation (before optimization)
if ! $OPT -load-pass-plugin="$PASS_LIB" \
    -passes=tripcount-annotation \
    -S "$TMP_DIR/input.ll" -o "$TMP_DIR/tripcount.ll"; then
    error "TripCountAnnotation pass failed"
fi

# Frontend optimization (if -Oc > 0)
MILP_INPUT_LL="$TMP_DIR/tripcount.ll"
if [[ "$CLANG_OPT_LEVEL" != "0" ]]; then
    if ! $OPT -passes="default<O$CLANG_OPT_LEVEL>" \
        -vectorize-loops=false -vectorize-slp=false \
        -S "$TMP_DIR/tripcount.ll" -o "$TMP_DIR/input_optimized.ll"; then
        error "IR optimization pipeline failed at -O$CLANG_OPT_LEVEL"
    fi
    MILP_INPUT_LL="$TMP_DIR/input_optimized.ll"
fi

# Helper: run assembly-based energy estimation on an IR file.
# Usage: run_assembly_energy <input.ll> <output_prefix> <energy_params_config>
# Outputs: <prefix>.bb_energy.json, <prefix>.bb_mapping.json
run_assembly_energy() {
    local input_ll="$1" prefix="$2" params_config="$3"

    if ! $OPT -load-pass-plugin="$BB_DEBUGINFO_LIB" \
        -passes=assign-bb-debuginfo \
        -bb-mapping="${prefix}.bb_mapping.json" \
        -S "$input_ll" -o "${prefix}.bbinfo.ll" 2>&1; then
        error "assign-bb-debuginfo pass failed for $input_ll"
    fi

    if ! $LLC -march=msp430 -filetype=obj "${prefix}.bbinfo.ll" -o "${prefix}.energy.o" 2>&1; then
        error "LLC compilation to object failed for ${prefix}.bbinfo.ll"
    fi

    if ! "$BB_ANALYZER" \
        --energy-params "$params_config" \
        --bb-mapping "${prefix}.bb_mapping.json" \
        "${prefix}.energy.o" > "${prefix}.bb_energy.json"; then
        error "bb-energy-analyzer failed for ${prefix}.energy.o"
    fi
}

# Helper: compile and run BB frequency profiling binary.
# Usage: collect_bb_freq <instrumented.ll> <output_bb_freq.json>
collect_bb_freq() {
    local inst_ll="$1" out_json="$2"

    # Strip MSP430 target triple so clang compiles a native binary for profiling.
    sed 's/^target triple = .*//' "$inst_ll" \
      | sed 's/^target datalayout = .*//' > "$TMP_DIR/freq_inst_native.ll"

    # Provide host stubs for MSP430-specific debug_init/debug_exit
    cat > "$TMP_DIR/debug_stubs.c" << 'STUBS'
void debug_init(void) {}
void debug_exit(int result) { (void)result; }
STUBS

    if ! $CLANG -O0 $_SYSROOT_FLAGS \
        "$TMP_DIR/freq_inst_native.ll" "$BB_FREQ_RUNTIME" "$TMP_DIR/debug_stubs.c" \
        -o "$TMP_DIR/freq_run" 2>&1; then
        error "BB frequency runtime compilation failed"
    fi

    (cd "$TMP_DIR" && ./freq_run) || true
    if [[ ! -f "$out_json" ]]; then
        error "BB frequency collection did not produce $(basename "$out_json")"
    fi
}

MILP_EXTRA_FLAGS=""
[[ "$ADD_DEBUG_MARKERS" == "true" ]] && MILP_EXTRA_FLAGS="-add-debug-markers"
[[ "$VERBOSE" == "true" ]] && MILP_EXTRA_FLAGS="$MILP_EXTRA_FLAGS -loop-strip-mining-verbose -abstract-cfg-verbose"

if [[ "$ESTIMATOR_MODE" == "assembly" ]]; then
    # ── Assembly-based two-pass energy estimation ────────────────────────────

    # Phase 2: Pre-strip-mining assembly energy
    info "Phase 2: Pre-strip-mining assembly energy..."
    run_assembly_energy "$MILP_INPUT_LL" "$TMP_DIR/pre" "$ESTIMATOR_CONFIG"

    # Generate temporary energy config pointing to pre-strip-mining energy data
    cat > "$TMP_DIR/pre_energy_config.json" << EOF
{"estimator_type": "assembly", "energy_data_path": "$TMP_DIR/pre.bb_energy.json"}
EOF

    # Phase 3: Preprocessing only (loop canonicalization + strip-mining)
    info "Phase 3: Preprocessing (LoopSimplify + LCSSA + LoopRotate + IndVarSimplify + LoopStripMining)..."
    if ! $OPT -load-pass-plugin="$PASS_LIB" \
        -passes=milp-preprocess \
        -energy-config="$TMP_DIR/pre_energy_config.json" \
        -milp-config="$MILP_CONFIG" \
        $([[ "$VERBOSE" == "true" ]] && echo "-loop-strip-mining-verbose") \
        -S "$MILP_INPUT_LL" -o "$TMP_DIR/preprocessed.ll" 2>&1; then
        error "MILP preprocessing pass failed"
    fi

    # Phase 4: Post-strip-mining assembly energy
    info "Phase 4: Post-strip-mining assembly energy..."
    run_assembly_energy "$TMP_DIR/preprocessed.ll" "$TMP_DIR/post" "$ESTIMATOR_CONFIG"

    # Generate temporary energy config pointing to post-strip-mining energy data
    cat > "$TMP_DIR/post_energy_config.json" << EOF
{"estimator_type": "assembly", "energy_data_path": "$TMP_DIR/post.bb_energy.json"}
EOF

    # Phase 5: BB frequency collection (on preprocessed IR, no re-preprocessing)
    PROFILE_START=$(_now_ms)
    info "Phase 5: Collecting BB frequencies..."
    if ! $OPT -load-pass-plugin="$PASS_LIB" \
        -passes=bb-freq-collect-only \
        -S "$TMP_DIR/preprocessed.ll" -o "$TMP_DIR/freq_inst.ll" 2>&1; then
        error "BB frequency collection pass failed"
    fi

    BB_FREQ_JSON="$TMP_DIR/bb_freq.json"
    collect_bb_freq "$TMP_DIR/freq_inst.ll" "$BB_FREQ_JSON"
    PROFILE_END=$(_now_ms)
    echo "Profiling time (ms): $((PROFILE_END - PROFILE_START))"

    # Phase 6: MILP solving only (on preprocessed IR, no re-preprocessing)
    info "Phase 6: MILP solving..."
    PASS_LOG=$(mktemp "$TMP_DIR/pass_XXXXXX.log")
    if $OPT -load-pass-plugin="$PASS_LIB" \
        -passes=milp-solve-only \
        -energy-config="$TMP_DIR/post_energy_config.json" \
        -milp-config="$MILP_CONFIG" \
        -bb-freq-file="$BB_FREQ_JSON" \
        $MILP_EXTRA_FLAGS \
        -S "$TMP_DIR/preprocessed.ll" -o "$TMP_DIR/ckpt.ll" \
        >"$PASS_LOG" 2>&1; then
        if [[ "$VERBOSE" == "true" ]]; then
            cat "$PASS_LOG"
        else
            head -10 "$PASS_LOG"
        fi
    else
        cat "$PASS_LOG" >&2
        error "MILP pass failed (see output above)"
    fi

else
    # ── IR-based single-pass energy estimation (existing flow) ───────────────

    # BB frequency collection: instrument, compile, run, get bb_freq.json
    PROFILE_START=$(_now_ms)
    info "Collecting BB frequencies..."
    if ! $OPT -load-pass-plugin="$PASS_LIB" \
        -passes=bb-freq-collect \
        -energy-config="$ESTIMATOR_CONFIG" \
        -milp-config="$MILP_CONFIG" \
        -S "$MILP_INPUT_LL" -o "$TMP_DIR/freq_inst.ll" 2>&1; then
        error "BB frequency collection pass failed"
    fi

    BB_FREQ_JSON="$TMP_DIR/bb_freq.json"
    collect_bb_freq "$TMP_DIR/freq_inst.ll" "$BB_FREQ_JSON"
    PROFILE_END=$(_now_ms)
    echo "Profiling time (ms): $((PROFILE_END - PROFILE_START))"

    # MILP pass
    PASS_LOG=$(mktemp "$TMP_DIR/pass_XXXXXX.log")
    if $OPT -load-pass-plugin="$PASS_LIB" \
        -passes=milp \
        -energy-config="$ESTIMATOR_CONFIG" \
        -milp-config="$MILP_CONFIG" \
        -bb-freq-file="$BB_FREQ_JSON" \
        $MILP_EXTRA_FLAGS \
        -S "$MILP_INPUT_LL" -o "$TMP_DIR/ckpt.ll" \
        >"$PASS_LOG" 2>&1; then
        if [[ "$VERBOSE" == "true" ]]; then
            cat "$PASS_LOG"
        else
            head -10 "$PASS_LOG"
        fi
    else
        cat "$PASS_LOG" >&2
        error "MILP pass failed (see output above)"
    fi
fi

# Output: compile to MSP430 object
$LLC -march=msp430 -O"$OPT_LEVEL" "$TMP_DIR/ckpt.ll" -o "$TMP_DIR/ckpt.s"
$GCC -mmcu=$DEVICE -msmall -I"$MSP430GCC_SUPPORT_PATH/include" \
    -c "$TMP_DIR/ckpt.s" -o "$TMP_DIR/ckpt.o"
cp "$TMP_DIR/ckpt.o" "${OUTPUT}.o"
cp "$TMP_DIR/ckpt.s" "${OUTPUT}.s"

# Optional: assemble + link
if [[ "$LINK" == "true" ]]; then
    echo "=== Assemble + Link ==="

    BOOT_SRC="$PROJECT_DIR/passes/runtime/milp_boot.S"
    RUNTIME_SRC="$PROJECT_DIR/passes/runtime/milp_runtime.c"
    LINKER_SCRIPT="$PROJECT_DIR/passes/runtime/milp_msp430fr5994.ld"

    [[ ! -f "$BOOT_SRC" ]] && error "$BOOT_SRC not found"
    [[ ! -f "$RUNTIME_SRC" ]] && error "$RUNTIME_SRC not found"
    [[ ! -f "$LINKER_SCRIPT" ]] && error "$LINKER_SCRIPT not found"

    BOOT_ASM_FLAGS=""
    case "$HALT_MODE" in
        bor)  BOOT_ASM_FLAGS="-DMILP_HALT_BOR" ;;
        lpm4) BOOT_ASM_FLAGS="-DMILP_HALT_LPM4" ;;
    esac
    [[ "$DEBUG_COUNTERS" == "true" ]] && BOOT_ASM_FLAGS="$BOOT_ASM_FLAGS -DDEBUG_COUNTERS"

    $GCC -mmcu=$DEVICE -msmall $BOOT_ASM_FLAGS -c "$BOOT_SRC" -o "${OUTPUT}.boot.o"
    $GCC -mmcu=$DEVICE -msmall -O2 \
        -I"$MSP430GCC_SUPPORT_PATH/include" \
        -I"$PROJECT_DIR/passes/runtime" \
        -c "$RUNTIME_SRC" -o "${OUTPUT}.runtime.o"

    LINK_OBJS=("${OUTPUT}.o" "${OUTPUT}.boot.o" "${OUTPUT}.runtime.o")

    if [[ "$DEBUG_COUNTERS" == "true" ]]; then
        DEBUG_SRC="$PROJECT_DIR/passes/runtime/milp_debug_counters.c"
        [[ ! -f "$DEBUG_SRC" ]] && error "$DEBUG_SRC not found"

        $GCC -mmcu=$DEVICE -msmall -O2 -DDEBUG_COUNTERS \
            -I"$MSP430GCC_SUPPORT_PATH/include" \
            -I"$PROJECT_DIR/passes/runtime" \
            -c "$DEBUG_SRC" -o "${OUTPUT}.debug_counters.o"

        LINK_OBJS+=("${OUTPUT}.debug_counters.o")
    fi

    $GCC -mmcu=$DEVICE -msmall \
        -L"$MSP430GCC_SUPPORT_PATH/include" \
        -T "$LINKER_SCRIPT" \
        -Wl,--nmagic \
        "${LINK_OBJS[@]}" -o "${OUTPUT}.elf"

    $SIZE "${OUTPUT}.elf"
fi

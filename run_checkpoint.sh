#!/bin/bash
#
# Checkpoint Insertion Script
# Compiles C files and runs the MILP checkpoint insertion pipeline
#

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Load environment variables from .env if it exists
if [[ -f "${SCRIPT_DIR}/.env" ]]; then
    source "${SCRIPT_DIR}/.env"
fi

# LLVM tools (use LLVM_DIR from .env or fall back to PATH)
if [[ -n "${LLVM_DIR}" ]]; then
    CLANG="${LLVM_DIR}/bin/clang"
    OPT="${LLVM_DIR}/bin/opt"
else
    CLANG="clang"
    OPT="opt"
fi

PASS_LIB="${SCRIPT_DIR}/passes/build/CheckpointPass.so"
DEFAULT_CONFIG="${SCRIPT_DIR}/benchmarks/sample_ir_energy_config.json"
DEFAULT_MILP_CONFIG="${SCRIPT_DIR}/benchmarks/sample_milp_config.json"
TMP_DIR="${SCRIPT_DIR}/tmp"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

usage() {
    cat << EOF
Usage: $(basename "$0") [OPTIONS] <input.c>

Compile a C file and run checkpoint insertion using the MILP algorithm.

OPTIONS:
    -o, --output <file>     Output file (default: tmp/<input>_checkpointed.ll)
    -c, --config <file>     Energy config JSON file (default: benchmarks/sample_ir_energy_config.json)
    -m, --milp-config <file> MILP config JSON file (default: benchmarks/sample_milp_config.json)
    -O, --opt-level <N>     Optimization level: 0, 1, 2, 3 (default: 3)
    -u, --unroll            Enable aggressive loop unrolling before checkpoint insertion
    -I, --include <dir>     Add include directory for compilation
    -k, --keep-intermediates Keep intermediate files (.ll)
    -v, --verbose           Verbose output
    -h, --help              Show this help message

EXAMPLES:
    $(basename "$0") test.c
    $(basename "$0") -O3 -u -o output.ll test.c
    $(basename "$0") -c custom_energy.json -m custom_milp.json -I ./include test.c

EOF
    exit 1
}

log() {
    echo -e "${GREEN}[checkpoint]${NC} $1"
}

warn() {
    echo -e "${YELLOW}[warning]${NC} $1"
}

error() {
    echo -e "${RED}[error]${NC} $1" >&2
    exit 1
}

# Default values
OUTPUT=""
CONFIG="${DEFAULT_CONFIG}"
MILP_CONFIG="${DEFAULT_MILP_CONFIG}"
OPT_LEVEL="3"
UNROLL=false
INCLUDES=()
KEEP_INTERMEDIATES=false
VERBOSE=false

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -o|--output)
            OUTPUT="$2"
            shift 2
            ;;
        -c|--config)
            CONFIG="$2"
            shift 2
            ;;
        -m|--milp-config)
            MILP_CONFIG="$2"
            shift 2
            ;;
        -O|--opt-level)
            OPT_LEVEL="$2"
            shift 2
            ;;
        -u|--unroll)
            UNROLL=true
            shift
            ;;
        -I|--include)
            INCLUDES+=("-I$2")
            shift 2
            ;;
        -k|--keep-intermediates)
            KEEP_INTERMEDIATES=true
            shift
            ;;
        -v|--verbose)
            VERBOSE=true
            shift
            ;;
        -h|--help)
            usage
            ;;
        -*)
            error "Unknown option: $1"
            ;;
        *)
            INPUT="$1"
            shift
            ;;
    esac
done

# Validate input
if [[ -z "${INPUT}" ]]; then
    error "No input file specified"
fi

if [[ ! -f "${INPUT}" ]]; then
    error "Input file not found: ${INPUT}"
fi

if [[ ! -f "${PASS_LIB}" ]]; then
    error "Checkpoint pass not found: ${PASS_LIB}\nPlease build the pass first (cd passes && mkdir build && cd build && cmake .. && make)"
fi

if [[ ! -f "${CONFIG}" ]]; then
    error "Energy config not found: ${CONFIG}"
fi

if [[ ! -f "${MILP_CONFIG}" ]]; then
    error "MILP config not found: ${MILP_CONFIG}"
fi

# Ensure tmp directory exists
mkdir -p "${TMP_DIR}"

# Determine output filename
BASENAME=$(basename "${INPUT}" .c)

if [[ -z "${OUTPUT}" ]]; then
    OUTPUT="${TMP_DIR}/${BASENAME}_checkpointed.ll"
fi

# Intermediate files (always in tmp/)
IR_FILE="${TMP_DIR}/${BASENAME}.ll"
if [[ "${UNROLL}" == true ]]; then
    IR_FILE="${TMP_DIR}/${BASENAME}_unrolled.ll"
fi

# Build clang flags
CLANG_FLAGS=("-S" "-emit-llvm" "-O${OPT_LEVEL}")

if [[ "${UNROLL}" == true ]]; then
    CLANG_FLAGS+=("-funroll-loops" "-mllvm" "-unroll-count=4" "-mllvm" "-unroll-threshold=10000")
else
    CLANG_FLAGS+=("-fno-unroll-loops")
fi

CLANG_FLAGS+=("${INCLUDES[@]}")

# Step 1: Compile C to LLVM IR
log "Compiling ${INPUT} to LLVM IR (O${OPT_LEVEL}, unroll=${UNROLL})"
if [[ "${VERBOSE}" == true ]]; then
    echo "  clang ${CLANG_FLAGS[*]} ${INPUT} -o ${IR_FILE}"
fi

"${CLANG}" "${CLANG_FLAGS[@]}" "${INPUT}" -o "${IR_FILE}" 2>&1

if [[ "${VERBOSE}" == true ]]; then
    BLOCK_COUNT=$(grep -c '^[0-9a-zA-Z_]*:' "${IR_FILE}" 2>/dev/null || true)
    log "Generated IR with ${BLOCK_COUNT} basic blocks"
fi

# Step 2: Run checkpoint insertion pass
log "Running checkpoint insertion pass"
if [[ "${VERBOSE}" == true ]]; then
    echo "  Using config: ${CONFIG}"
    echo "  Using MILP config: ${MILP_CONFIG}"
fi

"${OPT}" \
    -load-pass-plugin="${PASS_LIB}" \
    -passes=checkpoint-insert,milp-validate \
    -checkpoint-algorithm=milp \
    --energy-config="${CONFIG}" \
    --milp-config="${MILP_CONFIG}" \
    -S "${IR_FILE}" -o "${OUTPUT}" 2>&1

# Count results
BLOCK_COUNT=$(grep -c '^[0-9a-zA-Z_]*:' "${OUTPUT}" 2>/dev/null || true)
REGION_METADATA_COUNT=$(grep -c 'milp.region.starts' "${OUTPUT}" 2>/dev/null || true)

log "Done! MILP pipeline completed (with validation)"
log "Output: ${OUTPUT}"

if [[ "${VERBOSE}" == true ]]; then
    echo ""
    echo "=== Summary ==="
    echo "  Basic blocks:  ${BLOCK_COUNT}"
    echo "  Region metadata entries: ${REGION_METADATA_COUNT}"
    echo "  File size:     $(wc -c < "${OUTPUT}" | tr -d ' ') bytes"
    echo ""
    echo "=== Region metadata ==="
    grep "milp.region.starts" "${OUTPUT}" || true
fi

# Cleanup intermediate files
if [[ "${KEEP_INTERMEDIATES}" == false && "${IR_FILE}" != "${OUTPUT}" ]]; then
    rm -f "${IR_FILE}"
fi

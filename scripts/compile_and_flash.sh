#!/usr/bin/env bash
#
# Compile and Flash Script for MSP430FR5994
#
# Usage:
#   ./scripts/compile_and_flash.sh [options] <input.c>
#
# Options:
#   --mode <mode>    Checkpoint mode: none, rockclimb, milp (default: none)
#   --runtime <type> Runtime variant: real (default), mock-counter, energy-validate
#   --local          Compile for host machine instead of MSP430 (run locally)
#   --debug          Enable DEBUG output via UART
#   --compile-only   Compile but don't flash
#   --flash-only     Flash existing binary
#   -o <output>      Output base name (default: build/<input>)
#   -e <config>      Energy estimator config JSON (required for all modes)
#   -m <config>      MILP config JSON (required for milp mode)
#   -c <config>      RockClimb config JSON (required for rockclimb mode)
#   -O <level>       LLC optimization level (default: 2)
#   -Oc <level>      Clang optimization level (default: 2)
#   -I <dir>         Add include directory (can be repeated)
#   --verbose        Show detailed pass output
#   -h, --help       Show this help message
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Load environment
[[ -f "$PROJECT_DIR/.envrc" ]] && source "$PROJECT_DIR/.envrc" 2>/dev/null || true

# MSP430 toolchain
export MSP430GCC_TOOLCHAIN_PATH="${MSP430GCC_TOOLCHAIN_PATH:-$HOME/ti/msp430-gcc}"
export MSP430GCC_SUPPORT_PATH="${MSP430GCC_SUPPORT_PATH:-$HOME/ti/msp430-gcc}"
export PATH="$MSP430GCC_TOOLCHAIN_PATH/bin:$PATH"

# LLVM tools
CLANG="${LLVM_DIR:+$LLVM_DIR/bin/}clang"
OPT="${LLVM_DIR:+$LLVM_DIR/bin/}opt"
LLC="${LLVM_DIR:+$LLVM_DIR/bin/}llc"

# GCC tools
GCC="msp430-elf-gcc"
SIZE="msp430-elf-size"

# Defaults
MODE="none"
RUNTIME_TYPE="real"
RUNTIME_SET="false"
LOCAL_MODE="false"
DEBUG_MODE="false"
COMPILE_ONLY="false"
FLASH_ONLY="false"
VERBOSE="false"
OUTPUT=""
OPT_LEVEL=""
CLANG_OPT_LEVEL=""
EXTRA_INCLUDES=""
DEVICE="MSP430FR5994"
BUILD_DIR="$PROJECT_DIR/build"

PASS_LIB="$PROJECT_DIR/passes/build/CheckpointPass.so"

ESTIMATOR_CONFIG=""
MILP_CONFIG=""
ROCKCLIMB_CONFIG=""
SCHEMATIC_CONFIG=""

ROCKCLIMB_RUNTIME="$PROJECT_DIR/passes/runtime/rockclimb_runtime.c"
ROCKCLIMB_BOOT="$PROJECT_DIR/passes/runtime/rockclimb_boot.S"
ROCKCLIMB_MOCK_CKPT_COUNTER="$PROJECT_DIR/passes/runtime/rockclimb_mock_ckpt_counter.c"
ROCKCLIMB_LINKER="$PROJECT_DIR/passes/runtime/rockclimb_msp430fr5994.ld"

MILP_RUNTIME="$PROJECT_DIR/passes/runtime/milp_runtime.c"
MILP_BOOT="$PROJECT_DIR/passes/runtime/milp_boot.S"
MILP_MOCK_CKPT_COUNTER="$PROJECT_DIR/passes/runtime/milp_mock_ckpt_counter.c"
MILP_LINKER="$PROJECT_DIR/passes/runtime/milp_msp430fr5994.ld"

ENERGY_VALIDATE_RUNTIME="$PROJECT_DIR/passes/runtime/energy_validate_runtime.c"
VALIDATE_CKPT_FUNCTION=""

usage() { sed -n '2,26p' "$0" | sed 's/^# \?//'; exit 0; }
error() { echo -e "\033[0;31mError: $1\033[0m" >&2; exit 1; }
info() { echo -e "\033[0;36m$1\033[0m"; }

# Link checkpoint object with mock counter or real runtime.
# Usage: link_runtime <mock_counter> <runtime> <boot> <linker>
link_runtime() {
    local mock_counter="$1" runtime="$2" boot="$3" linker="$4"

    GCC_DEBUG_FLAGS=""
    [[ "$DEBUG_MODE" == "true" ]] && GCC_DEBUG_FLAGS="-DDEBUG"

    case "$RUNTIME_TYPE" in
    mock-counter)
        info "Using mock counter runtime (no power failure recovery)"
        $GCC -mmcu=$DEVICE -O2 -msmall $GCC_DEBUG_FLAGS \
            -I"$MSP430GCC_SUPPORT_PATH/include" \
            -I"$PROJECT_DIR/passes/runtime" \
            -c "$mock_counter" -o "$TMP_DIR/mock.o"

        $GCC -mmcu=$DEVICE -msmall -L"$MSP430GCC_SUPPORT_PATH/include" \
            "$TMP_DIR/ckpt.o" "$TMP_DIR/mock.o" -o "${OUTPUT}.elf"
        ;;
    real)
        info "Using real runtime (with power failure recovery)"
        $GCC -mmcu=$DEVICE -O2 -msmall $GCC_DEBUG_FLAGS \
            -I"$MSP430GCC_SUPPORT_PATH/include" \
            -I"$PROJECT_DIR/passes/runtime" \
            -c "$runtime" -o "$TMP_DIR/runtime.o"

        $GCC -mmcu=$DEVICE -msmall \
            -c "$boot" -o "$TMP_DIR/boot.o"

        $GCC -mmcu=$DEVICE -msmall -L"$MSP430GCC_SUPPORT_PATH/include" \
            -T "$linker" \
            "$TMP_DIR/ckpt.o" "$TMP_DIR/runtime.o" "$TMP_DIR/boot.o" \
            -o "${OUTPUT}.elf"
        ;;
    *)
        error "Unknown runtime type: $RUNTIME_TYPE (use: real, mock-counter)"
        ;;
    esac
}

# Compile LLVM IR to a host-native executable.
# Usage: link_local <ll_file> [mock_counter_source]
link_local() {
    local ll_file="$1"
    local mock_counter="${2:-}"
    local sysroot_flags=""

    # Source-built clang may not know the SDK path on macOS
    if command -v xcrun &>/dev/null; then
        sysroot_flags="-isysroot $(xcrun --show-sdk-path)"
    fi

    if [[ -n "$mock_counter" ]]; then
        $CLANG -O"$OPT_LEVEL" $sysroot_flags \
            -I"$PROJECT_DIR/passes/runtime" \
            "$ll_file" "$mock_counter" -o "${OUTPUT}"
    else
        $CLANG -O"$OPT_LEVEL" $sysroot_flags "$ll_file" -o "${OUTPUT}"
    fi
}

# Compile C source to LLVM IR.
compile_to_ir() {
    if [[ "$LOCAL_MODE" == "true" ]]; then
        CLANG_FLAGS="-S -emit-llvm -O$CLANG_OPT_LEVEL"
        CLANG_FLAGS="$CLANG_FLAGS -I$PROJECT_DIR/passes/include"
        # Source-built clang may not know the SDK path on macOS
        if command -v xcrun &>/dev/null; then
            CLANG_FLAGS="$CLANG_FLAGS -isysroot $(xcrun --show-sdk-path)"
        fi
    else
        CLANG_FLAGS="--target=msp430-elf -S -emit-llvm -O$CLANG_OPT_LEVEL -D__MSP430FR5994__"
        CLANG_FLAGS="$CLANG_FLAGS -I$PROJECT_DIR/passes/include -I$MSP430GCC_SUPPORT_PATH/include -I$MSP430GCC_SUPPORT_PATH/msp430-elf/include"
    fi
    [[ "$CLANG_OPT_LEVEL" == "0" ]] && \
        CLANG_FLAGS="$CLANG_FLAGS -Xclang -disable-O0-optnone"
    CLANG_FLAGS="$CLANG_FLAGS $EXTRA_INCLUDES"
    [[ "$DEBUG_MODE" == "true" ]] && CLANG_FLAGS="$CLANG_FLAGS -DDEBUG"
    $CLANG $CLANG_FLAGS "$INPUT" -o "$TMP_DIR/input.ll"
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode) MODE="$2"; shift 2 ;;
        --runtime) RUNTIME_TYPE="$2"; RUNTIME_SET="true"; shift 2 ;;
        --local) LOCAL_MODE="true"; shift ;;
        --debug) DEBUG_MODE="true"; shift ;;
        --compile-only) COMPILE_ONLY="true"; shift ;;
        --flash-only) FLASH_ONLY="true"; shift ;;
        --verbose) VERBOSE="true"; shift ;;
        -o) OUTPUT="$2"; shift 2 ;;
        -e|--energy-config) ESTIMATOR_CONFIG="$2"; shift 2 ;;
        -m|--milp-config) MILP_CONFIG="$2"; shift 2 ;;
        -c|--rockclimb-config) ROCKCLIMB_CONFIG="$2"; shift 2 ;;
        -s|--schematic-config) SCHEMATIC_CONFIG="$2"; shift 2 ;;
        -O) OPT_LEVEL="$2"; shift 2 ;;
        -Oc) CLANG_OPT_LEVEL="$2"; shift 2 ;;
        -I) EXTRA_INCLUDES="$EXTRA_INCLUDES -I$2"; shift 2 ;;
        --validate-checkpoint-function) VALIDATE_CKPT_FUNCTION="$2"; shift 2 ;;
        -h|--help) usage ;;
        -*) error "Unknown option: $1" ;;
        *) INPUT="$1"; shift ;;
    esac
done

# Validate --local flag combinations
if [[ "$LOCAL_MODE" == "true" ]]; then
    [[ "$FLASH_ONLY" == "true" ]] && \
        error "--local and --flash-only are incompatible"
    if [[ "$MODE" != "none" && "$RUNTIME_SET" != "true" ]]; then
        error "--local requires --runtime (e.g., --runtime mock-counter)"
    fi
    [[ "$RUNTIME_SET" == "true" && "$RUNTIME_TYPE" == "real" ]] && \
        error "--local is incompatible with --runtime real (real runtime needs MSP430 hardware)"
fi

# energy-validate requires --local (runs on host, not MSP430)
if [[ "$RUNTIME_TYPE" == "energy-validate" && "$LOCAL_MODE" != "true" ]]; then
    error "--runtime energy-validate requires --local (energy validation runs on host)"
fi

# Validate required configs per mode
if [[ "$FLASH_ONLY" != "true" ]]; then
    case "$MODE" in
        milp)
            [[ -z "$ESTIMATOR_CONFIG" ]] && error "Energy estimator config required for milp mode: use -e <config.json>"
            [[ ! -f "$ESTIMATOR_CONFIG" ]] && error "Estimator config not found: $ESTIMATOR_CONFIG"
            [[ -z "$MILP_CONFIG" ]] && error "MILP config required for milp mode: use -m <config.json>"
            [[ ! -f "$MILP_CONFIG" ]] && error "MILP config not found: $MILP_CONFIG"
            ;;
        rockclimb)
            [[ -z "$ESTIMATOR_CONFIG" ]] && error "Energy estimator config required for rockclimb mode: use -e <config.json>"
            [[ ! -f "$ESTIMATOR_CONFIG" ]] && error "Estimator config not found: $ESTIMATOR_CONFIG"
            [[ -z "$ROCKCLIMB_CONFIG" ]] && error "RockClimb config required: use -c <config.json>"
            [[ ! -f "$ROCKCLIMB_CONFIG" ]] && error "RockClimb config not found: $ROCKCLIMB_CONFIG"
            ;;
        schematic)
            [[ -z "$ESTIMATOR_CONFIG" ]] && error "Energy estimator config required for schematic mode: use -e <config.json>"
            [[ ! -f "$ESTIMATOR_CONFIG" ]] && error "Estimator config not found: $ESTIMATOR_CONFIG"
            [[ -z "$SCHEMATIC_CONFIG" ]] && error "SCHEMATIC config required: use -s <config.json>"
            [[ ! -f "$SCHEMATIC_CONFIG" ]] && error "SCHEMATIC config not found: $SCHEMATIC_CONFIG"
            ;;
        none)
            if [[ "$RUNTIME_TYPE" == "energy-validate" ]]; then
                [[ -z "$ESTIMATOR_CONFIG" ]] && error "Energy estimator config required for energy-validate: use -e <config.json>"
                [[ ! -f "$ESTIMATOR_CONFIG" ]] && error "Estimator config not found: $ESTIMATOR_CONFIG"
            fi
            ;;
    esac
fi

# Validate
[[ "$FLASH_ONLY" != "true" && -z "$INPUT" ]] && error "No input file specified"
[[ "$FLASH_ONLY" != "true" && ! -f "$INPUT" ]] && error "File not found: $INPUT"

# Output name
[[ -z "$OUTPUT" ]] && OUTPUT="$BUILD_DIR/$(basename "${INPUT:-.}" .c)"
mkdir -p "$(dirname "$OUTPUT")"

# Default optimization levels
[[ -z "$OPT_LEVEL" ]] && OPT_LEVEL="2"
[[ -z "$CLANG_OPT_LEVEL" ]] && CLANG_OPT_LEVEL="2"

TMP_DIR=$(mktemp -d)
trap "rm -rf $TMP_DIR" EXIT

echo "=========================================="
echo "Mode: $MODE | Runtime: $RUNTIME_TYPE | Target: $(if [[ "$LOCAL_MODE" == "true" ]]; then echo "host"; else echo "msp430"; fi) | Debug: $DEBUG_MODE"
echo "Output: $OUTPUT"
echo "=========================================="

if [[ "$FLASH_ONLY" != "true" ]]; then
    case "$MODE" in
        none)
            info "Compiling without checkpointing (via LLVM)..."

            compile_to_ir

            # Run energy-validate pass if requested (for user-defined checkpoints)
            if [[ "$RUNTIME_TYPE" == "energy-validate" ]]; then
                info "Running energy-validate pass..."
                VALIDATE_FLAGS="-energy-config=$ESTIMATOR_CONFIG"
                # For none mode with energy-validate, use milp-config if provided
                [[ -n "$MILP_CONFIG" ]] && VALIDATE_FLAGS="$VALIDATE_FLAGS -milp-config=$MILP_CONFIG"
                [[ -n "$VALIDATE_CKPT_FUNCTION" ]] && \
                    VALIDATE_FLAGS="$VALIDATE_FLAGS -validate-checkpoint-function=$VALIDATE_CKPT_FUNCTION"
                [[ "$VERBOSE" == "true" ]] && VALIDATE_FLAGS="$VALIDATE_FLAGS -validate-verbose"
                $OPT -load-pass-plugin="$PASS_LIB" \
                    -passes=energy-validate \
                    $VALIDATE_FLAGS \
                    -S "$TMP_DIR/input.ll" -o "$TMP_DIR/input.ll"
            fi

            if [[ "$LOCAL_MODE" == "true" ]]; then
                if [[ "$RUNTIME_TYPE" == "energy-validate" ]]; then
                    link_local "$TMP_DIR/input.ll" "$ENERGY_VALIDATE_RUNTIME"
                else
                    link_local "$TMP_DIR/input.ll"
                fi
            else
                # LLVM IR to assembly
                $LLC -march=msp430 -O"$OPT_LEVEL" "$TMP_DIR/input.ll" -o "$TMP_DIR/output.s"

                # Assemble and link with GCC
                $GCC -mmcu=$DEVICE -msmall -I"$MSP430GCC_SUPPORT_PATH/include" \
                    -L"$MSP430GCC_SUPPORT_PATH/include" \
                    "$TMP_DIR/output.s" -o "${OUTPUT}.elf"

                cp "$TMP_DIR/output.s" "${OUTPUT}.s"
            fi
            ;;

        rockclimb)
            info "Compiling with RockClimb..."
            [[ ! -f "$PASS_LIB" ]] && error "Pass not found: $PASS_LIB"

            compile_to_ir

            # RockClimb pass
            ROCKCLIMB_EXTRA_FLAGS=""
            if [[ "$RUNTIME_TYPE" == "mock-counter" ]]; then
                ROCKCLIMB_EXTRA_FLAGS="-add-debug-markers"
            fi
            PASS_LOG=$(mktemp "$TMP_DIR/pass_XXXXXX.log")
            if $OPT -load-pass-plugin="$PASS_LIB" \
                -passes=rockclimb \
                -energy-config="$ESTIMATOR_CONFIG" \
                -rockclimb-config="$ROCKCLIMB_CONFIG" \
                $ROCKCLIMB_EXTRA_FLAGS \
                -S "$TMP_DIR/input.ll" -o "$TMP_DIR/ckpt.ll" \
                >"$PASS_LOG" 2>&1; then
                if [[ "$VERBOSE" == "true" ]]; then
                    cat "$PASS_LOG"
                else
                    grep -E "^(Region|Memory|Inserted|===)" "$PASS_LOG" | head -10
                fi
            else
                cat "$PASS_LOG" >&2
                error "RockClimb pass failed (see output above)"
            fi

            # Run energy-validate pass if requested
            if [[ "$RUNTIME_TYPE" == "energy-validate" ]]; then
                info "Running energy-validate pass..."
                VALIDATE_FLAGS="-energy-config=$ESTIMATOR_CONFIG -rockclimb-config=$ROCKCLIMB_CONFIG -validate-mode=rockclimb"
                [[ "$VERBOSE" == "true" ]] && VALIDATE_FLAGS="$VALIDATE_FLAGS -validate-verbose"
                $OPT -load-pass-plugin="$PASS_LIB" \
                    -passes=energy-validate \
                    $VALIDATE_FLAGS \
                    -S "$TMP_DIR/ckpt.ll" -o "$TMP_DIR/ckpt.ll"
            fi

            if [[ "$LOCAL_MODE" == "true" ]]; then
                # Strip ELF-only .nvm section specifier (invalid on Mach-O)
                sed -i '' 's/, section ".nvm"//g' "$TMP_DIR/ckpt.ll"
                if [[ "$RUNTIME_TYPE" == "energy-validate" ]]; then
                    link_local "$TMP_DIR/ckpt.ll" "$ENERGY_VALIDATE_RUNTIME"
                else
                    link_local "$TMP_DIR/ckpt.ll" "$ROCKCLIMB_MOCK_CKPT_COUNTER"
                fi
            else
                # LLVM IR to assembly
                $LLC -march=msp430 -O"$OPT_LEVEL" "$TMP_DIR/ckpt.ll" -o "$TMP_DIR/ckpt.s"

                # Assemble with GCC
                $GCC -mmcu=$DEVICE -msmall -I"$MSP430GCC_SUPPORT_PATH/include" \
                    -c "$TMP_DIR/ckpt.s" -o "$TMP_DIR/ckpt.o"

                link_runtime "$ROCKCLIMB_MOCK_CKPT_COUNTER" "$ROCKCLIMB_RUNTIME" \
                    "$ROCKCLIMB_BOOT" "$ROCKCLIMB_LINKER"

                cp "$TMP_DIR/ckpt.s" "${OUTPUT}.s"
            fi
            ;;

        milp)
            info "Compiling with MILP..."
            [[ ! -f "$PASS_LIB" ]] && error "Pass not found: $PASS_LIB"

            # Run TripCountAnnotationPass before frontend-level -O optimizations.
            # This preserves loop marker placement before canonical LLVM passes
            # (unroll/peel/rotate) can move or erase source loops.
            MILP_FRONTEND_OPT_LEVEL="$CLANG_OPT_LEVEL"
            CLANG_OPT_LEVEL="0"
            compile_to_ir
            CLANG_OPT_LEVEL="$MILP_FRONTEND_OPT_LEVEL"

            if ! $OPT -load-pass-plugin="$PASS_LIB" \
                -passes=tripcount-annotation \
                -S "$TMP_DIR/input.ll" -o "$TMP_DIR/tripcount.ll"; then
                error "TripCountAnnotation pass failed"
            fi

            MILP_INPUT_LL="$TMP_DIR/tripcount.ll"
            if [[ "$MILP_FRONTEND_OPT_LEVEL" != "0" ]]; then
                if ! $OPT -passes="default<O$MILP_FRONTEND_OPT_LEVEL>" \
                    -vectorize-loops=false -vectorize-slp=false \
                    -S "$TMP_DIR/tripcount.ll" -o "$TMP_DIR/input_optimized.ll"; then
                    error "IR optimization pipeline failed at -O$MILP_FRONTEND_OPT_LEVEL"
                fi
                MILP_INPUT_LL="$TMP_DIR/input_optimized.ll"
            fi

            # MILP pass
            MILP_EXTRA_FLAGS=""
            if [[ "$RUNTIME_TYPE" == "mock-counter" ]]; then
                MILP_EXTRA_FLAGS="-add-debug-markers"
            fi
            PASS_LOG=$(mktemp "$TMP_DIR/pass_XXXXXX.log")
            if $OPT -load-pass-plugin="$PASS_LIB" \
                -passes=milp \
                -energy-config="$ESTIMATOR_CONFIG" \
                -milp-config="$MILP_CONFIG" \
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

            # Run energy-validate pass if requested
            if [[ "$RUNTIME_TYPE" == "energy-validate" ]]; then
                info "Running energy-validate pass..."
                VALIDATE_FLAGS="-energy-config=$ESTIMATOR_CONFIG -milp-config=$MILP_CONFIG -validate-mode=milp"
                [[ "$VERBOSE" == "true" ]] && VALIDATE_FLAGS="$VALIDATE_FLAGS -validate-verbose"
                $OPT -load-pass-plugin="$PASS_LIB" \
                    -passes=energy-validate \
                    $VALIDATE_FLAGS \
                    -S "$TMP_DIR/ckpt.ll" -o "$TMP_DIR/ckpt.ll"
            fi

            if [[ "$LOCAL_MODE" == "true" ]]; then
                # Strip ELF-only .nvm section specifier (invalid on Mach-O)
                sed -i '' 's/, section ".nvm"//g' "$TMP_DIR/ckpt.ll"
                if [[ "$RUNTIME_TYPE" == "energy-validate" ]]; then
                    link_local "$TMP_DIR/ckpt.ll" "$ENERGY_VALIDATE_RUNTIME"
                else
                    link_local "$TMP_DIR/ckpt.ll" "$MILP_MOCK_CKPT_COUNTER"
                fi
            else
                # LLVM IR to assembly
                $LLC -march=msp430 -O"$OPT_LEVEL" "$TMP_DIR/ckpt.ll" -o "$TMP_DIR/ckpt.s"

                # Assemble with GCC
                $GCC -mmcu=$DEVICE -msmall -I"$MSP430GCC_SUPPORT_PATH/include" \
                    -c "$TMP_DIR/ckpt.s" -o "$TMP_DIR/ckpt.o"

                link_runtime "$MILP_MOCK_CKPT_COUNTER" "$MILP_RUNTIME" \
                    "$MILP_BOOT" "$MILP_LINKER"

                cp "$TMP_DIR/ckpt.s" "${OUTPUT}.s"
            fi
            ;;

        schematic)
            info "Compiling with SCHEMATIC..."
            [[ ! -f "$PASS_LIB" ]] && error "Pass not found: $PASS_LIB"

            # Two-phase compile: annotate trip counts at -O0, then optimize.
            # This preserves loop structure for __loop_tripcount annotation
            # before LLVM optimizations can unroll/eliminate loops.
            SCHEMATIC_FRONTEND_OPT_LEVEL="$CLANG_OPT_LEVEL"
            CLANG_OPT_LEVEL="0"
            compile_to_ir
            CLANG_OPT_LEVEL="$SCHEMATIC_FRONTEND_OPT_LEVEL"

            if ! $OPT -load-pass-plugin="$PASS_LIB" \
                -passes=tripcount-annotation \
                -S "$TMP_DIR/input.ll" -o "$TMP_DIR/tripcount.ll"; then
                error "TripCountAnnotation pass failed"
            fi

            SCHEMATIC_INPUT_LL="$TMP_DIR/tripcount.ll"
            if [[ "$SCHEMATIC_FRONTEND_OPT_LEVEL" != "0" ]]; then
                if ! $OPT -passes="default<O$SCHEMATIC_FRONTEND_OPT_LEVEL>" \
                    -vectorize-loops=false -vectorize-slp=false \
                    -S "$TMP_DIR/tripcount.ll" -o "$TMP_DIR/input_optimized.ll"; then
                    error "IR optimization pipeline failed at -O$SCHEMATIC_FRONTEND_OPT_LEVEL"
                fi
                SCHEMATIC_INPUT_LL="$TMP_DIR/input_optimized.ll"
            fi

            SCHEMATIC_EXTRA_FLAGS=""
            if [[ "$RUNTIME_TYPE" == "mock-counter" ]]; then
                SCHEMATIC_EXTRA_FLAGS="-add-debug-markers"
            fi
            PASS_LOG=$(mktemp "$TMP_DIR/pass_XXXXXX.log")
            if $OPT -load-pass-plugin="$PASS_LIB" \
                -passes=schematic \
                -energy-config="$ESTIMATOR_CONFIG" \
                -schematic-config="$SCHEMATIC_CONFIG" \
                $SCHEMATIC_EXTRA_FLAGS \
                -S "$SCHEMATIC_INPUT_LL" -o "$TMP_DIR/ckpt.ll" \
                >"$PASS_LOG" 2>&1; then
                if [[ "$VERBOSE" == "true" ]]; then
                    cat "$PASS_LOG"
                else
                    head -10 "$PASS_LOG"
                fi
            else
                cat "$PASS_LOG" >&2
                error "SCHEMATIC pass failed (see output above)"
            fi

            if [[ "$LOCAL_MODE" == "true" ]]; then
                sed -i '' 's/, section ".nvm"//g' "$TMP_DIR/ckpt.ll"
                link_local "$TMP_DIR/ckpt.ll" "$MILP_MOCK_CKPT_COUNTER"
            else
                $LLC -march=msp430 -O"$OPT_LEVEL" "$TMP_DIR/ckpt.ll" -o "$TMP_DIR/ckpt.s"
                $GCC -mmcu=$DEVICE -msmall -I"$MSP430GCC_SUPPORT_PATH/include" \
                    -c "$TMP_DIR/ckpt.s" -o "$TMP_DIR/ckpt.o"
                link_runtime "$MILP_MOCK_CKPT_COUNTER" "$MILP_RUNTIME" \
                    "$MILP_BOOT" "$MILP_LINKER"
                cp "$TMP_DIR/ckpt.s" "${OUTPUT}.s"
            fi
            ;;

        *)
            error "Unknown mode: $MODE (use: none, rockclimb, milp, schematic)"
            ;;
    esac

    if [[ "$LOCAL_MODE" == "true" ]]; then
        size "${OUTPUT}" 2>/dev/null || true
    else
        $SIZE "${OUTPUT}.elf"
    fi
fi

if [[ "$COMPILE_ONLY" != "true" ]]; then
    if [[ "$LOCAL_MODE" == "true" ]]; then
        info "Running locally..."
        "${OUTPUT}" || true
    else
        info "Flashing..."
        mspdebug tilib "prog ${OUTPUT}.elf"
        echo ""
        [[ "$DEBUG_MODE" == "true" ]] && echo "Serial: screen /dev/tty.usbmodem* 9600"
    fi
fi

echo "Done."

#!/usr/bin/env bash
#
# Shared infrastructure for checkpoint compilation scripts.
# Source this file; guarded against double-sourcing.
#

[[ -n "${_COMMON_SH_LOADED:-}" ]] && return 0
_COMMON_SH_LOADED=1

set -e

# ── Project paths ────────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[1]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Load environment
[[ -f "$PROJECT_DIR/.envrc" ]] && source "$PROJECT_DIR/.envrc" 2>/dev/null || true

# ── Toolchain paths ──────────────────────────────────────────────────────────

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

# MSP430 device
DEVICE="MSP430FR5994"

# Pass plugin
PASS_LIB="$PROJECT_DIR/passes/build/CheckpointPass.so"

# ── Runtime file paths ───────────────────────────────────────────────────────

ROCKCLIMB_RUNTIME="$PROJECT_DIR/passes/runtime/rockclimb_runtime.c"
ROCKCLIMB_BOOT="$PROJECT_DIR/passes/runtime/rockclimb_boot.S"
ROCKCLIMB_MOCK_CKPT_COUNTER="$PROJECT_DIR/passes/runtime/rockclimb_mock_ckpt_counter.c"
ROCKCLIMB_LINKER="$PROJECT_DIR/passes/runtime/rockclimb_msp430fr5994.ld"

MILP_RUNTIME="$PROJECT_DIR/passes/runtime/milp_runtime.c"
MILP_BOOT="$PROJECT_DIR/passes/runtime/milp_boot.S"
MILP_MOCK_CKPT_COUNTER="$PROJECT_DIR/passes/runtime/milp_mock_ckpt_counter.c"
MILP_LINKER="$PROJECT_DIR/passes/runtime/milp_msp430fr5994.ld"

SCHEMATIC_TRACE_RUNTIME="$PROJECT_DIR/passes/runtime/schematic_trace_runtime.c"
SCHEMATIC_MOCK_CKPT_COUNTER="$PROJECT_DIR/passes/runtime/schematic_mock_ckpt_counter.c"

ENERGY_VALIDATE_RUNTIME="$PROJECT_DIR/passes/runtime/energy_validate_runtime.c"
BB_FREQ_RUNTIME="$PROJECT_DIR/passes/runtime/bb_freq_runtime.c"

# ── macOS SDK detection (cached) ─────────────────────────────────────────────

_SYSROOT_FLAGS=""
if command -v xcrun &>/dev/null; then
    _SYSROOT_FLAGS="-isysroot $(xcrun --show-sdk-path)"
fi

# ── Utility functions ────────────────────────────────────────────────────────

error() { echo -e "\033[0;31mError: $1\033[0m" >&2; exit 1; }
info()  { echo -e "\033[0;36m$1\033[0m"; }
_now_ms() { python3 -c 'import time; print(int(time.time() * 1000))'; }

# ── compile_to_ir ────────────────────────────────────────────────────────────
#
# Compile C source to LLVM IR.
#
# Usage: compile_to_ir <input.c> <output.ll> [--local] [--clang-opt-level N]
#                      [--debug] [-I dir ...]
#
compile_to_ir() {
    local input="" output="" local_mode="false" clang_opt_level="2" debug="false"
    local extra_includes=""

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --local) local_mode="true"; shift ;;
            --clang-opt-level) clang_opt_level="$2"; shift 2 ;;
            --debug) debug="true"; shift ;;
            -I) extra_includes="$extra_includes -I$2"; shift 2 ;;
            *)
                if [[ -z "$input" ]]; then
                    input="$1"
                elif [[ -z "$output" ]]; then
                    output="$1"
                else
                    error "compile_to_ir: unexpected argument: $1"
                fi
                shift
                ;;
        esac
    done

    [[ -z "$input" ]]  && error "compile_to_ir: no input file"
    [[ -z "$output" ]] && error "compile_to_ir: no output file"

    local flags
    if [[ "$local_mode" == "true" ]]; then
        flags="-S -emit-llvm -O$clang_opt_level"
        flags="$flags -I$PROJECT_DIR/passes/include"
        [[ -n "$_SYSROOT_FLAGS" ]] && flags="$flags $_SYSROOT_FLAGS"
    else
        flags="--target=msp430-elf -S -emit-llvm -O$clang_opt_level -D__MSP430FR5994__"
        flags="$flags -I$PROJECT_DIR/passes/include -I$MSP430GCC_SUPPORT_PATH/include -I$MSP430GCC_SUPPORT_PATH/msp430-elf/include"
    fi

    [[ "$clang_opt_level" == "0" ]] && flags="$flags -Xclang -disable-O0-optnone"
    flags="$flags $extra_includes"
    [[ "$debug" == "true" ]] && flags="$flags -DDEBUG"

    $CLANG $flags "$input" -o "$output"
}

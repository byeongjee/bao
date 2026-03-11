#!/usr/bin/env bash
#
# Shared infrastructure for checkpoint compilation scripts.
# Source this file; guarded against double-sourcing.
#

[[ -n "${_COMMON_SH_LOADED:-}" ]] && return 0
_COMMON_SH_LOADED=1

set -e

# ── Project paths ────────────────────────────────────────────────────────────

_COMMON_SH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
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

ROCKCLIMB_MOCK_CKPT_COUNTER="$PROJECT_DIR/passes/runtime/rockclimb_mock_ckpt_counter.c"

MILP_RUNTIME="$PROJECT_DIR/passes/runtime/milp_runtime.c"
MILP_BOOT="$PROJECT_DIR/passes/runtime/milp_boot.S"
MILP_MOCK_CKPT_COUNTER="$PROJECT_DIR/passes/runtime/milp_mock_ckpt_counter.c"
MILP_LINKER="$PROJECT_DIR/passes/runtime/milp_msp430fr5994.ld"

SCHEMATIC_TRACE_RUNTIME="$PROJECT_DIR/passes/runtime/schematic_trace_runtime.c"
SCHEMATIC_MOCK_CKPT_COUNTER="$PROJECT_DIR/passes/runtime/schematic_mock_ckpt_counter.c"
SCHEMATIC_RUNTIME="$PROJECT_DIR/passes/runtime/schematic_runtime.c"
SCHEMATIC_BOOT="$PROJECT_DIR/passes/runtime/schematic_boot.S"
SCHEMATIC_LINKER="$PROJECT_DIR/passes/runtime/rockclimb_msp430fr5994.ld"
SCHEMATIC_DEBUG_COUNTERS="$PROJECT_DIR/passes/runtime/schematic_debug_counters.c"

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
    local extra_includes="" extra_defines=""

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --local) local_mode="true"; shift ;;
            --clang-opt-level) clang_opt_level="$2"; shift 2 ;;
            --debug) debug="true"; shift ;;
            -I) extra_includes="$extra_includes -I$2"; shift 2 ;;
            -D) extra_defines="$extra_defines -D$2"; shift 2 ;;
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
        flags="$flags -I$PROJECT_DIR/passes/include -isystem $MSP430GCC_SUPPORT_PATH/include -isystem $MSP430GCC_SUPPORT_PATH/msp430-elf/include"
    fi

    [[ "$clang_opt_level" == "0" ]] && flags="$flags -Xclang -disable-O0-optnone"
    flags="$flags $extra_includes $extra_defines"
    [[ "$debug" == "true" ]] && flags="$flags -DDEBUG"

    $CLANG $flags "$input" -o "$output"
}

# ── flash_run_and_read ──────────────────────────────────────────────────────
#
# Flash an ELF, run the program, and read NVM symbols via mspdebug md.
#
# Usage: flash_run_and_read <elf> <timeout> <symbol1> [symbol2 ...]
# Output: key=value pairs on stdout (one per symbol)
#
# Single mspdebug session: flash, set HW breakpoint at __nvm_breakpoint,
# run (stops when program writes NVM values), then read NVM via md.
# Works for both baseline and RockClimb modes.
#
flash_run_and_read() {
    local elf="$1" timeout="$2"
    shift 2
    local symbols=("$@")

    local sym_csv
    sym_csv=$(IFS=,; echo "${symbols[*]}")

    local read_nvm_script="$_COMMON_SH_DIR/read_nvm.py"

    # Get breakpoint address: __nvm_breakpoint is called right after NVM writes.
    # Setting a HW breakpoint here makes mspdebug "run" return after the program
    # stores results, so we can read NVM in the same session.
    local bkpt_addr
    bkpt_addr=$(msp430-elf-nm "$elf" | awk '/ __nvm_breakpoint$/{print "0x"$1}')
    if [[ -z "$bkpt_addr" ]]; then
        echo "Error: __nvm_breakpoint symbol not found in $elf" >&2
        return 1
    fi

    # Compute md command from symbol addresses
    local md_cmd md_err
    md_err=$(mktemp)
    md_cmd=$(uv run python3 "$read_nvm_script" --elf "$elf" --symbols "$sym_csv" --md-cmd-only 2>"$md_err") || {
        echo "Error: Failed to compute md command" >&2
        cat "$md_err" >&2
        rm -f "$md_err"
        return 1
    }
    rm -f "$md_err"

    if [[ -z "$md_cmd" ]]; then
        echo "Error: md command is empty" >&2
        return 1
    fi

    # Single mspdebug session: flash → set breakpoint → run → read NVM
    echo "  [flash_run_and_read] bkpt=$bkpt_addr md='$md_cmd'" >&2
    local md_output
    local mspdebug_rc=0
    md_output=$(timeout "$timeout" mspdebug tilib \
        "prog $elf" \
        "setbreak $bkpt_addr" \
        "run" \
        "$md_cmd" 2>&1) || mspdebug_rc=$?

    if [[ $mspdebug_rc -ne 0 ]]; then
        echo "Error: mspdebug session failed (exit code $mspdebug_rc)" >&2
        echo "  Last 5 lines of mspdebug output:" >&2
        echo "$md_output" | tail -5 >&2
        return 1
    fi

    # Parse hex dump from mspdebug output and extract symbol values
    echo "$md_output" | uv run python3 "$read_nvm_script" --elf "$elf" --symbols "$sym_csv" --parse-md
}

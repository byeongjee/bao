#!/usr/bin/env bash
#
# Test-compile all UlSWaP-Bench benchmarks through the LLVM→MSP430 pipeline.
# Reports which benchmarks succeed and which fail at each stage.
#
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/src"

if [[ -z "$LLVM_DIR" ]]; then
    echo "Error: LLVM_DIR not set." >&2
    exit 1
fi
CLANG="$LLVM_DIR/bin/clang"
LLC="$LLVM_DIR/bin/llc"

MSP430GCC="${MSP430GCC_TOOLCHAIN_PATH:-$HOME/ti/msp430-gcc}"
GCC="$MSP430GCC/bin/msp430-elf-gcc"
SIZE="$MSP430GCC/bin/msp430-elf-size"

COMMON_SRCS="$SRC_DIR/common.c $SRC_DIR/mainmain.c $SRC_DIR/checksum.c"
INCLUDE_DIRS="-I$SRC_DIR -I$MSP430GCC/include -I$MSP430GCC/msp430-elf/include"

CLANG_FLAGS="--target=msp430-elf -S -emit-llvm -O2 -D__MSP430FR5994__ -DNO_PRINT -DNDEBUG $INCLUDE_DIRS"
LLC_FLAGS="-march=msp430 -O2"
GCC_FLAGS="-mmcu=msp430fr5994 -msmall -L$MSP430GCC/include"

TMP_DIR=$(mktemp -d)
trap "rm -rf $TMP_DIR" EXIT

# Extract source files from CMakeLists.txt for a benchmark
get_sources() {
    local bench_dir="$1"
    local cmake="$bench_dir/CMakeLists.txt"
    if [[ ! -f "$cmake" ]]; then
        echo ""
        return
    fi
    # Extract .c files from target_sources() lines
    grep -oE '\$\{CMAKE_CURRENT_LIST_DIR\}/[a-zA-Z0-9_]+\.c' "$cmake" \
        | sed "s|\${CMAKE_CURRENT_LIST_DIR}|$bench_dir|g"
}

# Get extra compile flags from CMakeLists.txt
get_extra_flags() {
    local bench_dir="$1"
    local cmake="$bench_dir/CMakeLists.txt"
    if [[ ! -f "$cmake" ]]; then
        echo ""
        return
    fi
    # Extract -D flags and -W flags
    grep -oE '\-[DW][a-zA-Z0-9_=\-]+' "$cmake" 2>/dev/null | tr '\n' ' '
}

# Check if benchmark links against math library
needs_math() {
    local bench_dir="$1"
    local cmake="$bench_dir/CMakeLists.txt"
    grep -q 'target_link_libraries.*\bm\b' "$cmake" 2>/dev/null
}

echo "============================================"
echo "UlSWaP-Bench LLVM→MSP430 Compilation Test"
echo "============================================"
echo "LLVM: $($CLANG --version 2>&1 | head -1)"
echo "GCC:  $($GCC --version 2>&1 | head -1)"
echo ""

declare -A RESULTS
BENCHMARKS=$(ls -d "$SRC_DIR"/*/CMakeLists.txt 2>/dev/null | sed 's|.*/src/||;s|/CMakeLists.txt||' | sort)

for BENCH in $BENCHMARKS; do
    BENCH_DIR="$SRC_DIR/$BENCH"
    WORK="$TMP_DIR/$BENCH"
    mkdir -p "$WORK"

    SOURCES=$(get_sources "$BENCH_DIR")
    EXTRA=$(get_extra_flags "$BENCH_DIR")

    if [[ -z "$SOURCES" ]]; then
        RESULTS[$BENCH]="SKIP: no sources found"
        continue
    fi

    ALL_SRCS="$SOURCES $COMMON_SRCS"
    OBJ_FILES=""
    FAILED=""
    FAIL_STAGE=""
    FAIL_MSG=""

    for SRC in $ALL_SRCS; do
        BASE=$(basename "$SRC" .c)
        # Avoid duplicate compilation of common files
        LL_FILE="$WORK/${BASE}.ll"
        S_FILE="$WORK/${BASE}.s"
        O_FILE="$WORK/${BASE}.o"

        if [[ -f "$LL_FILE" ]]; then
            OBJ_FILES="$OBJ_FILES $O_FILE"
            continue
        fi

        # Stage 1: clang → IR
        CLANG_OUT=$($CLANG $CLANG_FLAGS $EXTRA -I"$BENCH_DIR" "$SRC" -o "$LL_FILE" 2>&1)
        if [[ $? -ne 0 ]]; then
            FAILED=1
            FAIL_STAGE="clang"
            FAIL_MSG="$CLANG_OUT"
            break
        fi

        # Stage 2: llc → assembly
        LLC_OUT=$($LLC $LLC_FLAGS "$LL_FILE" -o "$S_FILE" 2>&1)
        if [[ $? -ne 0 ]]; then
            FAILED=1
            FAIL_STAGE="llc"
            FAIL_MSG="$LLC_OUT"
            break
        fi

        # Stage 3: gcc assemble → object
        GCC_ASM_OUT=$($GCC -mmcu=msp430fr5994 -msmall -c "$S_FILE" -o "$O_FILE" 2>&1)
        if [[ $? -ne 0 ]]; then
            FAILED=1
            FAIL_STAGE="gcc-asm"
            FAIL_MSG="$GCC_ASM_OUT"
            break
        fi

        OBJ_FILES="$OBJ_FILES $O_FILE"
    done

    if [[ -n "$FAILED" ]]; then
        RESULTS[$BENCH]="FAIL at $FAIL_STAGE: $(echo "$FAIL_MSG" | head -5)"
        printf "%-20s FAIL (%s)\n" "$BENCH" "$FAIL_STAGE"
        continue
    fi

    # Stage 4: link
    MATH_FLAG=""
    needs_math "$BENCH_DIR" && MATH_FLAG="-lm"
    LINK_OUT=$($GCC $GCC_FLAGS $OBJ_FILES $MATH_FLAG -o "$WORK/$BENCH.elf" 2>&1)
    if [[ $? -ne 0 ]]; then
        RESULTS[$BENCH]="FAIL at link: $(echo "$LINK_OUT" | head -5)"
        printf "%-20s FAIL (link)\n" "$BENCH"
        continue
    fi

    # Success - get size
    SIZE_OUT=$($SIZE "$WORK/$BENCH.elf" 2>&1 | tail -1)
    TEXT=$(echo "$SIZE_OUT" | awk '{print $1}')
    DATA=$(echo "$SIZE_OUT" | awk '{print $2}')
    BSS=$(echo "$SIZE_OUT" | awk '{print $3}')
    TOTAL=$(echo "$SIZE_OUT" | awk '{print $4}')
    RESULTS[$BENCH]="OK (text=$TEXT data=$DATA bss=$BSS total=$TOTAL)"
    printf "%-20s OK   (total=%s bytes)\n" "$BENCH" "$TOTAL"
done

echo ""
echo "============================================"
echo "DETAILED RESULTS"
echo "============================================"
for BENCH in $(echo "${!RESULTS[@]}" | tr ' ' '\n' | sort); do
    printf "%-20s %s\n" "$BENCH" "${RESULTS[$BENCH]}"
done

echo ""
echo "============================================"
echo "SUMMARY"
echo "============================================"
OK=0; FAIL=0; SKIP=0
for BENCH in "${!RESULTS[@]}"; do
    case "${RESULTS[$BENCH]}" in
        OK*) ((OK++)) ;;
        FAIL*) ((FAIL++)) ;;
        SKIP*) ((SKIP++)) ;;
    esac
done
echo "Passed: $OK  Failed: $FAIL  Skipped: $SKIP  Total: $((OK+FAIL+SKIP))"

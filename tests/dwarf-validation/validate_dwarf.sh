#!/bin/bash
# DWARF Parsing Validation Script
# Validates that the DWARF parsing correctly maps IR basic blocks to assembly address ranges

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUTPUT_DIR="$SCRIPT_DIR/output"

# Source .env if it exists
if [ -f "$PROJECT_ROOT/.env" ]; then
    export $(grep -v '^#' "$PROJECT_ROOT/.env" | xargs)
fi

# Check LLVM_DIR
if [ -z "$LLVM_DIR" ]; then
    echo -e "${RED}Error: LLVM_DIR not set. Please set it in .env or environment.${NC}"
    exit 1
fi

# Set up tool paths
CLANG="$LLVM_DIR/bin/clang"
OPT="$LLVM_DIR/bin/opt"
LLC="$LLVM_DIR/bin/llc"

PASS_PLUGIN="$PROJECT_ROOT/passes/build/bb-debuginfo/BBDebugInfoPass.so"
BB_ANALYZER="$PROJECT_ROOT/passes/build/bb-energy-analyzer/bb-energy-analyzer"

# MSP430 toolchain
if [ -n "$MSP430_GCC_DIR" ]; then
    MSP430_OBJDUMP="$MSP430_GCC_DIR/bin/msp430-elf-objdump"
else
    MSP430_OBJDUMP="/Users/byeongjee/ti/msp430-gcc/bin/msp430-elf-objdump"
fi

# Verify tools exist
echo -e "${BLUE}Checking prerequisites...${NC}"
missing_tools=0

for tool in "$CLANG" "$OPT" "$LLC"; do
    if [ ! -x "$tool" ]; then
        echo -e "${RED}  Missing: $tool${NC}"
        missing_tools=1
    fi
done

if [ ! -f "$PASS_PLUGIN" ]; then
    echo -e "${RED}  Missing: BBDebugInfoPass.so at $PASS_PLUGIN${NC}"
    echo "  Please build: cd passes/build && make"
    missing_tools=1
fi

if [ ! -x "$MSP430_OBJDUMP" ]; then
    echo -e "${RED}  Missing: msp430-elf-objdump at $MSP430_OBJDUMP${NC}"
    missing_tools=1
fi

if [ $missing_tools -eq 1 ]; then
    exit 1
fi

echo -e "${GREEN}  All tools found${NC}"
echo ""

# Create output directory
mkdir -p "$OUTPUT_DIR"

# Function to count BBs in labeled IR
count_ir_bbs() {
    local ir_file="$1"
    local func_name="$2"

    # Method: Count basic block labels within the function body
    # In LLVM IR, the entry block is unlabeled (counted as BB 0)
    # Other blocks appear as labels like "6:" at the start of a line
    # Extract function body between "define ... @func_name(" and the closing "}"

    local in_func=0
    local bb_count=0

    while IFS= read -r line; do
        # Check for function start
        if [[ "$line" =~ ^define.*@${func_name}\( ]]; then
            in_func=1
            bb_count=1  # Entry block (unlabeled)
            continue
        fi

        if [ $in_func -eq 1 ]; then
            # Check for function end (closing brace)
            if [[ "$line" =~ ^} ]]; then
                break
            fi
            # Count BB labels (number followed by colon at start of line)
            if [[ "$line" =~ ^[0-9]+:[[:space:]] ]]; then
                ((bb_count++))
            fi
        fi
    done < "$ir_file"

    echo "$bb_count"
}

# Function to extract and annotate disassembly
generate_annotated_disasm() {
    local obj_file="$1"
    local output_file="$2"

    # Get resolved address->BB mapping using bb-energy-analyzer
    # This applies heuristics to resolve line 0 (unmapped) entries
    local addr_map=$(mktemp)
    "$BB_ANALYZER" --dump-line-map "$obj_file" > "$addr_map" 2>/dev/null

    # Get disassembly
    local disasm=$("$MSP430_OBJDUMP" -d "$obj_file" 2>/dev/null)

    # Annotate disassembly with BB markers
    {
        local current_bb=""
        local current_func=""

        echo "$disasm" | while IFS= read -r line; do
            # Check for function label: 00000000 <function_name>:
            if [[ "$line" =~ ^([0-9a-fA-F]+)[[:space:]]+\<([^.][^>]*)\>: ]]; then
                current_func="${BASH_REMATCH[2]}"
                echo ""
                echo "$line"
                current_bb=""
                continue
            fi

            # Check for instruction: "   addr: bytes  mnemonic"
            if [[ "$line" =~ ^[[:space:]]*([0-9a-fA-F]+): ]]; then
                local addr="${BASH_REMATCH[1]}"
                # Pad address for lookup
                local padded_addr=$(printf "%08x" "0x$addr")

                # Look up BB for this address
                local bb_info=$(grep "^$padded_addr " "$addr_map" 2>/dev/null | head -1)
                if [ -n "$bb_info" ]; then
                    local new_bb=$(echo "$bb_info" | awk '{print $2}')
                    if [ "$new_bb" != "$current_bb" ]; then
                        if [ "$new_bb" = "0" ]; then
                            echo -e "     ${CYAN}---- UNMAPPED (line 0) ----${NC}"
                        else
                            echo -e "     ${CYAN}---- BB $new_bb (line $new_bb) ----${NC}"
                        fi
                        current_bb="$new_bb"
                    fi
                fi
            fi

            echo "$line"
        done
    } > "$output_file"

    rm -f "$addr_map"
}

# Function to count unique BBs from DWARF line table
count_dwarf_bbs() {
    local obj_file="$1"
    local func_start="$2"
    local func_end="$3"
    local max_bb="${4:-999999}"  # Optional: max valid BB number from IR

    # Get line table and count unique line numbers within function range
    # Line 0 = unmapped (prologue/epilogue), excluded from count
    # Line N (1 <= N <= max_bb) = BB N, counted
    # Line > max_bb = original source line from DISubprogram, excluded
    "$MSP430_OBJDUMP" --dwarf=decodedline "$obj_file" 2>/dev/null | \
        grep -E "^[^[:space:]]+[[:space:]]+[0-9]+[[:space:]]+0x" | \
        while IFS= read -r line; do
            if [[ "$line" =~ ^[^[:space:]]+[[:space:]]+([0-9]+)[[:space:]]+0x([0-9a-fA-F]+) ]]; then
                local bb_num="${BASH_REMATCH[1]}"
                local addr=$((16#${BASH_REMATCH[2]}))
                # Only count valid BB line numbers: 1 <= bb_num <= max_bb
                if [ "$bb_num" -gt 0 ] && [ "$bb_num" -le "$max_bb" ] && \
                   [ "$addr" -ge "$func_start" ] && [ "$addr" -lt "$func_end" ]; then
                    echo "$bb_num"
                fi
            fi
        done | sort -n -u | wc -l
}

# Function to get function address range from disassembly
get_func_range() {
    local obj_file="$1"
    local func_name="$2"

    local disasm=$("$MSP430_OBJDUMP" -d "$obj_file" 2>/dev/null)

    # Find function start - format is "00000000 <func_name>:"
    local func_line=$(echo "$disasm" | grep -E "^[0-9a-fA-F]+ <${func_name}>:" | head -1)
    local func_start=$(echo "$func_line" | awk '{print $1}')

    if [ -z "$func_start" ]; then
        echo "0 0"
        return
    fi

    # Find all function labels
    local all_funcs=$(echo "$disasm" | grep -E "^[0-9a-fA-F]+ <[^.][^>]*>:" | awk '{print $1}')
    local found=0
    local func_end=""

    for addr in $all_funcs; do
        if [ $found -eq 1 ]; then
            func_end="$addr"
            break
        fi
        if [ "$addr" == "$func_start" ]; then
            found=1
        fi
    done

    # If no next function, find last instruction address
    if [ -z "$func_end" ]; then
        func_end=$(echo "$disasm" | grep -E "^[[:space:]]+[0-9a-fA-F]+:" | tail -1 | sed 's/^[[:space:]]*//' | cut -d: -f1)
        func_end=$(printf "%x" $((16#$func_end + 4)))
    fi

    echo "$((16#$func_start)) $((16#$func_end))"
}

# Process a single test file
process_test() {
    local src_file="$1"
    local test_name=$(basename "$src_file" .c)

    echo -e "${YELLOW}================================================================${NC}"
    echo -e "${YELLOW}=== $test_name ===${NC}"
    echo -e "${YELLOW}================================================================${NC}"
    echo ""

    local ll_file="$OUTPUT_DIR/${test_name}.ll"
    local labeled_ll="$OUTPUT_DIR/${test_name}.labeled.ll"
    local obj_file="$OUTPUT_DIR/${test_name}.o"
    local annotated_file="$OUTPUT_DIR/${test_name}.annotated.txt"

    # Step 1: Compile C to LLVM IR (no -g flag needed - we create fresh debug info)
    echo -e "${BLUE}Step 1: Compile to LLVM IR (-O0, no debug info)${NC}"
    "$CLANG" -S -emit-llvm -O0 -Xclang -disable-O0-optnone \
             "$src_file" -o "$ll_file" 2>&1 | sed 's/^/  /'
    echo "  Output: $ll_file"
    echo ""

    # Step 2: Run BB debug info pass (creates fresh debug info from scratch)
    # No need to strip - the pass creates its own DICompileUnit and DISubprograms
    echo -e "${BLUE}Step 2: Run assign-bb-debuginfo pass${NC}"
    echo "  (Creates fresh debug info: Line 0 = unmapped, Lines 1+ = basic blocks)"
    "$OPT" -load-pass-plugin="$PASS_PLUGIN" \
           -passes=assign-bb-debuginfo \
           -S "$ll_file" -o "$labeled_ll" 2>&1 | sed 's/^/  /'
    echo "  Output: $labeled_ll"
    echo ""

    # Step 3: Compile to MSP430 object file
    echo -e "${BLUE}Step 3: Compile to MSP430 object file${NC}"
    "$LLC" -march=msp430 -filetype=obj "$labeled_ll" -o "$obj_file" 2>&1 | sed 's/^/  /'
    echo "  Output: $obj_file"
    echo ""

    # Step 4: Generate annotated disassembly
    echo -e "${BLUE}Step 4: Annotated Disassembly (BB boundaries from DWARF line numbers)${NC}"
    echo "------------------------------------------------------------"
    generate_annotated_disasm "$obj_file" "$annotated_file"
    cat "$annotated_file"
    echo ""

    # Step 5: Show DWARF line table
    echo -e "${BLUE}Step 5: DWARF Line Table (raw)${NC}"
    echo "  File                Line(BB)  Address"
    echo "  ------------------  --------  -------"
    "$MSP430_OBJDUMP" --dwarf=decodedline "$obj_file" 2>/dev/null | \
        grep -E "^[^[:space:]]+[[:space:]]+[0-9]+[[:space:]]+0x" | head -30 | \
        awk '{printf "  %-18s %-9s %s\n", $1, $2, $3}'
    echo ""

    # Step 6: Comparison
    echo -e "${BLUE}Step 6: COMPARISON${NC}"
    echo "------------------------------------------------------------"

    # Find function names in the file
    local funcs=$(grep -E "^define.*@[a-zA-Z_][a-zA-Z0-9_]*" "$labeled_ll" | \
                  sed -E 's/.*@([a-zA-Z_][a-zA-Z0-9_]*)\(.*/\1/')

    for func in $funcs; do
        echo ""
        echo "  Function: $func"

        # Count BBs in labeled IR
        local ir_bbs=$(count_ir_bbs "$labeled_ll" "$func")
        echo "    IR BBs (from labeled.ll):    $ir_bbs"

        # Get function range and count DWARF BBs
        local range=$(get_func_range "$obj_file" "$func")
        local func_start=$(echo "$range" | awk '{print $1}')
        local func_end=$(echo "$range" | awk '{print $2}')

        if [ -n "$func_start" ] && [ "$func_start" != "" ]; then
            local dwarf_bbs=$(count_dwarf_bbs "$obj_file" "$func_start" "$func_end")
            echo "    DWARF BBs (from line table): $dwarf_bbs"

            # Compare - trim whitespace for comparison
            local ir_trimmed=$(echo "$ir_bbs" | tr -d '[:space:]')
            local dwarf_trimmed=$(echo "$dwarf_bbs" | tr -d '[:space:]')

            if [ "$ir_trimmed" -eq "$dwarf_trimmed" ]; then
                echo -e "    ${GREEN}Counts match${NC}"
            else
                echo -e "    ${RED}Counts MISMATCH (IR=$ir_trimmed, DWARF=$dwarf_trimmed)${NC}"
            fi
        else
            echo "    DWARF BBs: (function not found in object)"
        fi
    done

    echo ""
    echo ""
}

# Main execution
echo "========================================"
echo "DWARF Parsing Validation Test Suite"
echo "========================================"
echo ""
echo "This script validates that the DWARF parsing"
echo "correctly maps IR basic blocks to assembly addresses."
echo ""
echo "Line number encoding:"
echo "  Line 0 = Unmapped (prologue, epilogue, compiler-generated code)"
echo "  Line N = Basic block N (N >= 1)"
echo ""

# Process all test files
for src in "$SCRIPT_DIR"/test_*.c; do
    if [ -f "$src" ]; then
        process_test "$src"
    fi
done

echo "========================================"
echo "Validation Complete"
echo "========================================"
echo ""
echo "Output files are in: $OUTPUT_DIR"
echo ""
echo "Files per test:"
echo "  *.ll           - Original IR (no debug info)"
echo "  *.labeled.ll   - IR with fresh debug info (BB indices as line numbers)"
echo "  *.o            - MSP430 object file with DWARF"
echo "  *.annotated.txt - Annotated disassembly"
echo ""
echo "Manual Verification Checklist:"
echo "  [ ] BB count in IR matches BB count in DWARF line table"
echo "  [ ] Each BB's address range groups correct instructions"
echo "  [ ] Disassembly instructions within each range are logical"
echo "  [ ] No instructions are attributed to wrong BB"
echo "========================================"

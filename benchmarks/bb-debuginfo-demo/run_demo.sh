#!/bin/bash
# Demo script for AssignBBDebugInfoPass
# Shows how to map IR basic blocks to assembly addresses via DWARF

set -e

# Load environment
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Source .env if it exists
if [ -f "$PROJECT_ROOT/.env" ]; then
    export $(grep -v '^#' "$PROJECT_ROOT/.env" | xargs)
fi

# Check LLVM_DIR
if [ -z "$LLVM_DIR" ]; then
    echo "Error: LLVM_DIR not set. Please set it in .env or environment."
    exit 1
fi

CLANG="$LLVM_DIR/bin/clang"
OPT="$LLVM_DIR/bin/opt"
LLC="$LLVM_DIR/bin/llc"
LLVM_DWARFDUMP="$LLVM_DIR/bin/llvm-dwarfdump"
LLVM_OBJDUMP="$LLVM_DIR/bin/llvm-objdump"

PASS_PLUGIN="$PROJECT_ROOT/passes/build/bb-debuginfo/BBDebugInfoPass.so"

# Check tools exist
for tool in "$CLANG" "$OPT" "$LLC" "$LLVM_DWARFDUMP"; do
    if [ ! -x "$tool" ]; then
        echo "Error: $tool not found or not executable"
        exit 1
    fi
done

if [ ! -f "$PASS_PLUGIN" ]; then
    echo "Error: BBDebugInfoPass.so not found at $PASS_PLUGIN"
    echo "Please build the pass first: cd passes/build && make"
    exit 1
fi

# Create output directory
OUTPUT_DIR="$SCRIPT_DIR/output"
mkdir -p "$OUTPUT_DIR"

# Process a single C file
process_file() {
    local src="$1"
    local name=$(basename "$src" .c)

    echo "========================================"
    echo "Processing: $name.c"
    echo "========================================"

    # Step 1: Compile to IR with debug info
    echo ""
    echo "Step 1: Compile to LLVM IR with debug info (-O2 -g)"
    "$CLANG" -O2 -g -emit-llvm -S "$src" -o "$OUTPUT_DIR/${name}.ll"
    echo "  Output: $OUTPUT_DIR/${name}.ll"

    # Step 2: Run BB debug info pass
    echo ""
    echo "Step 2: Run assign-bb-debuginfo pass"
    "$OPT" -load-pass-plugin="$PASS_PLUGIN" \
           -passes=assign-bb-debuginfo \
           -S "$OUTPUT_DIR/${name}.ll" -o "$OUTPUT_DIR/${name}_labeled.ll" 2>&1 | sed 's/^/  /'
    echo "  Output: $OUTPUT_DIR/${name}_labeled.ll"

    # Step 3: Compile to object file
    echo ""
    echo "Step 3: Compile to object file"
    "$LLC" -O2 -filetype=obj "$OUTPUT_DIR/${name}_labeled.ll" -o "$OUTPUT_DIR/${name}.o"
    echo "  Output: $OUTPUT_DIR/${name}.o"

    # Step 4: Show DWARF line table
    echo ""
    echo "Step 4: DWARF line table (BB index = line number)"
    echo "  Address            Line(BB)  Column  File"
    echo "  ------------------ --------  ------  ----"
    "$LLVM_DWARFDUMP" --debug-line "$OUTPUT_DIR/${name}.o" 2>/dev/null | \
        grep -E "^0x[0-9a-f]+" | \
        awk '{printf "  %-18s %-8s %-7s %s\n", $1, $2, $3, $4}'

    # Step 5: Show disassembly with source annotations
    echo ""
    echo "Step 5: Disassembly with BB annotations"
    "$LLVM_OBJDUMP" -d --source "$OUTPUT_DIR/${name}.o" 2>/dev/null | head -50

    echo ""
}

# Main
echo "BB Debug Info Pass Demo"
echo "======================="
echo ""
echo "This demo shows how to map IR basic blocks to assembly addresses."
echo "The pass assigns line numbers equal to BB indices (0, 1, 2, ...)."
echo "DWARF debug info then maps assembly addresses to these line numbers."
echo ""

# Process all C files in the demo directory
for src in "$SCRIPT_DIR"/*.c; do
    if [ -f "$src" ]; then
        process_file "$src"
    fi
done

echo "========================================"
echo "Demo complete!"
echo ""
echo "Output files are in: $OUTPUT_DIR"
echo ""
echo "Key files:"
echo "  *.ll         - Original IR with debug info"
echo "  *_labeled.ll - IR with BB indices as line numbers"
echo "  *.o          - Object file with DWARF"
echo ""
echo "To explore further:"
echo "  llvm-dwarfdump --debug-line output/<name>.o"
echo "  llvm-objdump -d --source output/<name>.o"
echo "========================================"

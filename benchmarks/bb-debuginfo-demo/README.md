# BB Debug Info Pass Demo

This demo shows how the `AssignBBDebugInfoPass` maps IR basic blocks to assembly addresses using DWARF debug information.

## How It Works

1. **Compile C to LLVM IR** with debug info (`-g` flag)
2. **Run the pass** which assigns `line = BB_index` to each instruction
3. **Compile to object file** preserving debug info
4. **DWARF line table** now maps assembly addresses → BB indices

## Running the Demo

```bash
# From this directory
./run_demo.sh

# Or from project root
./benchmarks/bb-debuginfo-demo/run_demo.sh
```

## Output

The script processes each `.c` file and shows:
- Number of instructions labeled per function
- DWARF line table mapping addresses to BB indices
- Disassembly with source annotations

Output files are written to the `output/` directory:
- `*.ll` - Original IR with debug info
- `*_labeled.ll` - IR with BB indices as line numbers
- `*.o` - Object file with DWARF

## Example Output

For `loop.c`:
```
DWARF line table (BB index = line number)
Address            Line(BB)  Column  File
------------------  --------  ------  ----
0x0000000000000000  0         0       ...   <- BB 0 (entry)
0x0000000000000008  1         0       ...   <- BB 1 (loop preheader)
0x0000000000000020  2         0       ...   <- BB 2 (exit)
```

## Test Files

| File | Description | Expected BBs (after -O2) |
|------|-------------|--------------------------|
| `simple_branch.c` | if/else | 1 (optimized to select) |
| `loop.c` | for loop | 3 |
| `nested_branch.c` | nested if/else | 1 (optimized) |
| `switch_case.c` | switch statement | 3 |

Note: With `-O2` optimization, many branches get converted to branchless `select` or `csel` instructions, reducing the number of basic blocks.

## Manual Exploration

```bash
# View labeled IR
cat output/loop_labeled.ll

# Check DWARF line table
$LLVM_DIR/bin/llvm-dwarfdump --debug-line output/loop.o

# Disassemble with source
$LLVM_DIR/bin/llvm-objdump -d --source output/loop.o
```

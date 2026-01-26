# DWARF Parsing Validation Tests

This directory contains tests to validate that the DWARF parsing correctly maps IR basic blocks to assembly address ranges.

## Overview

The validation pipeline:

1. **AssignBBDebugInfoPass** - Assigns unique line numbers (0, 1, 2, ...) to each IR basic block
2. **DWARF Debug Info** - Preserved through compilation to MSP430 object code
3. **DWARFParser** - Extracts (address, line) pairs from object file
4. **Mapping** - Line number becomes BB index, address ranges group instructions

## Test Cases

| Test | CFG Pattern | Expected BBs | Purpose |
|------|-------------|--------------|---------|
| `test_single_bb.c` | Straight-line | 1 BB | Simplest case - no control flow |
| `test_if_else.c` | Diamond (if/else) | 4 BBs | Entry, then-block, else-block, merge |
| `test_simple_loop.c` | Loop | 3-4 BBs | Preheader, header, body, exit |
| `test_two_functions.c` | Multiple functions | 2+ BBs each | Per-function BB mapping |

## Running the Validation

```bash
# Make sure tools are built first
cd passes/build && make

# Run validation
./validate_dwarf.sh
```

## Prerequisites

- LLVM (set `LLVM_DIR` in `.env` or environment)
- MSP430-GCC toolchain (set `MSP430_GCC_DIR` or uses default path)
- Built `BBDebugInfoPass.so`

## Output Files

The script generates these files in `output/`:

| File | Description |
|------|-------------|
| `*.ll` | Original LLVM IR with debug info |
| `*.labeled.ll` | IR with BB indices as line numbers |
| `*.o` | MSP430 object file with DWARF |
| `*.annotated.txt` | Disassembly with BB boundary markers |

## What the Annotated Output Shows

```
0000000000000000 <test_if_else>:
   ---- BB 0 (line 0) ----
        0: 0b 12         push  r11
        2: 0a 12         push  r10
   ---- BB 1 (line 1) ----
        6: 91 93 00 00   cmp   #0, 0(sp)
        a: 04 38         jl    $+10
   ---- BB 2 (line 2) ----
        c: 1f 41 00 00   mov   0(sp), r15
       10: 3f 50 0a 00   add   #10, r15
   ...
```

The `---- BB N ----` markers indicate where the DWARF line table shows a change in BB index.

## Manual Verification Checklist

For each test, verify:

- [ ] BB count in labeled IR matches BB count in DWARF line table
- [ ] Each BB's address range in DWARF correctly groups related instructions
- [ ] Disassembly instructions within each range make sense for that BB
- [ ] No instructions are attributed to the wrong BB
- [ ] Control flow instructions (jumps, branches) appear at BB boundaries

## Success Criteria

The validation passes when:

1. **Counts match**: IR BB count = DWARF BB count for each function
2. **Ranges correct**: Address ranges group logically related instructions
3. **No errors**: Mapping errors would show instructions in wrong BBs

## Troubleshooting

### "msp430-elf-objdump not found"

Set `MSP430_GCC_DIR` to your TI MSP430-GCC installation:

```bash
export MSP430_GCC_DIR=/path/to/ti/msp430-gcc
```

### BB count mismatch

This can happen if:
- Compiler optimizations merge or split blocks
- Debug info is incomplete
- The pass doesn't assign all blocks a line number

Check the `*.labeled.ll` file to see which blocks have `!dbg` metadata.

### Missing BB markers in disassembly

The DWARF line table may not have entries for all addresses. This is normal - the parser uses the last known line number until a new entry appears.

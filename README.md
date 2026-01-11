# MILP-based Checkpoint Insertion for LLVM IR

An LLVM pass that automatically inserts checkpoints into programs for intermittent computing. The pass uses Mixed-Integer Linear Programming (MILP) to find optimal checkpoint placements that minimize runtime overhead while guaranteeing energy constraints are satisfied.

## Overview

This tool analyzes LLVM IR, builds a control flow graph with energy cost estimates, solves an optimization problem to determine where checkpoints should be placed, and instruments the IR with checkpoint function calls.

## Requirements

- **LLVM 22+** (built from source)
- **Gurobi Optimizer** (with valid license - free for academic use)
- **CMake 3.20+**
- **C++17 compatible compiler**

## Project Structure

```
checkpoint-insertion/
├── passes/                    # C++ LLVM pass implementation
│   ├── CMakeLists.txt
│   ├── include/
│   │   ├── EnergyModel.h      # Instruction energy costs
│   │   ├── CFGAnalysis.h      # CFG construction with loop info
│   │   ├── CheckpointOptimizer.h  # Gurobi MILP solver
│   │   └── CheckpointPass.h   # Main LLVM pass
│   └── src/
│       ├── EnergyModel.cpp
│       ├── CFGAnalysis.cpp
│       ├── CheckpointOptimizer.cpp
│       └── CheckpointPass.cpp
└── src/                       # Python reference implementation
    ├── main.py
    ├── ir_parser.py
    ├── optimizer.py
    └── energy_model.py
```

## Building

### 1. Set up environment variables

```bash
export LLVM_DIR=/path/to/llvm-project/build
export GUROBI_HOME=/path/to/gurobi  # e.g., /Library/gurobi1300/macos_universal2
export PATH=$LLVM_DIR/bin:$PATH
```

### 2. Build the pass

```bash
cd passes
mkdir build && cd build
cmake .. -DLLVM_DIR=$LLVM_DIR/lib/cmake/llvm
make
```

This produces `CheckpointPass.so`.

## Usage

### 1. Compile C to LLVM IR

```bash
clang -S -emit-llvm -O0 -Xclang -disable-O0-optnone input.c -o input.ll
```

> **Note:** The `-Xclang -disable-O0-optnone` flag is required to allow optimization passes to run on `-O0` compiled code.

### 2. Run the checkpoint pass

```bash
opt -load-pass-plugin=./CheckpointPass.so \
    -passes=checkpoint \
    -checkpoint-capacity=100 \
    -checkpoint-function=__checkpoint \
    -S input.ll -o instrumented.ll
```

### Command-line Options

| Option | Description | Default |
|--------|-------------|---------|
| `-checkpoint-capacity=<N>` | Energy capacity between checkpoints | 100 |
| `-checkpoint-function=<name>` | Name of checkpoint function to call | `__checkpoint` |

### 3. Link with checkpoint implementation

The pass inserts calls to `void __checkpoint(const char* block_name)`. Provide your own implementation:

```c
// checkpoint_impl.c
#include <stdio.h>

void __checkpoint(const char* block_name) {
    // Save program state here
    printf("Checkpoint at: %s\n", block_name);
}
```

Compile and link:

```bash
clang instrumented.ll checkpoint_impl.c -o program
```

## Example

```bash
# Create test program
cat > test.c << 'EOF'
int sum_squares(int n) {
    int total = 0;
    for (int i = 0; i < n; i++) {
        total += i * i;
    }
    return total;
}
int main() { return sum_squares(100); }
EOF

# Compile to IR
clang -S -emit-llvm -O0 -Xclang -disable-O0-optnone test.c -o test.ll

# Run checkpoint insertion
opt -load-pass-plugin=./passes/build/CheckpointPass.so \
    -passes=checkpoint \
    -checkpoint-capacity=50 \
    -S test.ll -o instrumented.ll

# View inserted checkpoints
grep "__checkpoint" instrumented.ll
```

## Energy Model

The pass estimates energy costs for LLVM IR instructions.


## License

MIT License

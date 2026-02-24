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
│   └── include/
│   ├── include/
│   │   ├── EnergyEstimator.h       # Abstract energy estimation interface
│   │   ├── IRBasedEstimator.h      # IR-based energy estimator
│   │   ├── EnergyEstimatorFactory.h # Factory for creating estimators
│   │   ├── CFGAnalysis.h           # CFG construction with loop info
│   │   ├── CheckpointOptimizer.h   # Gurobi MILP solver
│   │   ├── CheckpointPass.h        # Main LLVM pass
│   │   ├── LoopTripCount.h         # Loop bound extraction
│   │   └── LoopStripMiningPass.h   # Loop strip-mining pass
│   └── src/
│       ├── IRBasedEstimator.cpp
│       ├── EnergyEstimatorFactory.cpp
│       ├── CFGAnalysis.cpp
│       ├── CheckpointOptimizer.cpp
│       ├── CheckpointPass.cpp
│       ├── LoopTripCount.cpp
│       └── LoopStripMiningPass.cpp
├── tests/                     # Test suite
│   ├── run_tests.sh
│   └── *.c                    # Test cases
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

MILP now requires profile-guided block frequencies. Generate profile data first,
then compile IR with `-fprofile-instr-use`:

```bash
# 1) Build + run training binary
clang -O0 -Xclang -disable-O0-optnone \
    -fprofile-instr-generate=default.profraw \
    input.c -o input_train
LLVM_PROFILE_FILE=default.profraw ./input_train || true

# 2) Merge profile data
llvm-profdata merge -o default.profdata default.profraw

# 3) Emit profiled LLVM IR
clang -S -emit-llvm -O0 -Xclang -disable-O0-optnone \
    -fprofile-instr-use=default.profdata \
    input.c -o input.ll
```

> **Note:** The `-Xclang -disable-O0-optnone` flag is required to allow optimization passes to run on `-O0` compiled code.

### 2. Run the checkpoint pass

```bash
opt -load-pass-plugin=./CheckpointPass.so \
    -passes=milp \
    -energy-config=./benchmarks/sample_ir_energy_config.json \
    -milp-config=./tests/milp_params.json \
    -checkpoint-function=__milp_checkpoint \
    -S input.ll -o instrumented.ll
```

> **Note:** `-passes=checkpoint` is still supported; `-passes=milp` is a clearer alias for the MILP-based pass.
> **Note:** MILP fails fast if profile metadata is missing in the input IR.

### Command-line Options

| Option | Description | Default |
|--------|-------------|---------|
| `-energy-config=<path>` | Path to JSON energy configuration file | (required) |
| `-checkpoint-function=<name>` | Name of checkpoint function to call | `__checkpoint` |

### 3. Link with checkpoint implementation

The pass inserts calls to a checkpoint function with signature `void fn(const char* block_name)`.

For MILP mode, the recommended symbol name is `__milp_checkpoint` (and the repo also provides embedded runtime stubs under `passes/runtime/`).

Provide your own implementation:

```c
// checkpoint_impl.c
#include <stdio.h>

void __milp_checkpoint(const char* block_name) {
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

# Build/run training binary, then emit profiled IR
clang -O0 -Xclang -disable-O0-optnone \
    -fprofile-instr-generate=test.profraw \
    test.c -o test_train
LLVM_PROFILE_FILE=test.profraw ./test_train || true
llvm-profdata merge -o test.profdata test.profraw
clang -S -emit-llvm -O0 -Xclang -disable-O0-optnone \
    -fprofile-instr-use=test.profdata \
    test.c -o test.ll

# Run checkpoint insertion
opt -load-pass-plugin=./passes/build/CheckpointPass.so \
    -passes=milp \
    -energy-config=./benchmarks/sample_ir_energy_config.json \
    -milp-config=./tests/milp_params.json \
    -checkpoint-function=__milp_checkpoint \
    -S test.ll -o instrumented.ll

# View inserted checkpoints
grep "__milp_checkpoint" instrumented.ll
```

## Energy Model

The energy estimation system uses a pluggable architecture that allows different estimation strategies.

### Configuration File

Energy parameters are specified in a JSON configuration file. The `estimator_type` field is **required** to ensure explicit configuration:

```json
{
  "estimator_type": "ir",
  "energy_parameters": {
    "capacity": 100.0,
    "instruction_costs": {
      "simple_arithmetic": 1,
      "complex_arithmetic": 3,
      "floating_point": 5,
      "load": 4,
      "store": 5,
      "control_flow": 1,
      "comparison": 1,
      "conversion": 2,
      "call": 10,
      "phi_select": 0,
      "gep": 1,
      "alloca": 1,
      "atomic": 10,
      "default": 1
    }
  }
}
```

### Instruction Categories

| Category | LLVM Instructions |
|----------|-------------------|
| `simple_arithmetic` | add, sub, and, or, xor, shl, lshr, ashr |
| `complex_arithmetic` | mul, sdiv, udiv, srem, urem |
| `floating_point` | fadd, fsub, fmul, fdiv, frem |
| `load` | load |
| `store` | store |
| `control_flow` | br, switch, ret, indirectbr |
| `comparison` | icmp, fcmp |
| `conversion` | trunc, zext, sext, fptoui, fptosi, uitofp, sitofp, fptrunc, fpext, ptrtoint, inttoptr, bitcast, addrspacecast |
| `call` | call, invoke |
| `phi_select` | phi, select |
| `gep` | getelementptr |
| `alloca` | alloca |
| `atomic` | atomicrmw, cmpxchg, fence |

### Built-in Estimators

- **`ir`**: Estimates energy by summing instruction costs from LLVM IR

## Extending with Custom Estimators

The energy estimation system is designed to be extensible. You can add custom estimators (e.g., assembly-based, ML-based) without modifying existing code.

### Step 1: Implement the EnergyEstimator Interface

Create a new class that inherits from `EnergyEstimator`:

```cpp
// passes/src/AssemblyBasedEstimator.cpp
#include "EnergyEstimator.h"
#include "EnergyEstimatorFactory.h"

namespace checkpoint {

class AssemblyBasedEstimator : public EnergyEstimator {
public:
    explicit AssemblyBasedEstimator(const std::string &configPath) {
        // Load configuration and initialize
    }

    EnergyEstimate estimate(const llvm::BasicBlock &BB) override {
        // Your custom energy estimation logic
        double cost = /* compute cost */;
        return EnergyEstimate{cost, "assembly-based"};
    }

    double getCapacity() const override {
        return capacity_;
    }

    std::string getName() const override {
        return "assembly-based";
    }

private:
    double capacity_;
};

} // namespace checkpoint
```

### Step 2: Register with the Factory

Add your estimator type to `EnergyEstimatorFactory::createDefault()` in `passes/src/EnergyEstimatorFactory.cpp`:

```cpp
EnergyEstimatorFactory EnergyEstimatorFactory::createDefault() {
    EnergyEstimatorFactory factory;
    // Built-in IR-based estimator
    factory.registerType("ir", [](const std::string &configPath) {
        return std::make_unique<IRBasedEstimator>(configPath);
    });
    // Add your custom estimator
    factory.registerType("assembly", [](const std::string &configPath) {
        return std::make_unique<AssemblyBasedEstimator>(configPath);
    });
    return factory;
}
```

### Step 3: Update CMakeLists.txt

Add your new source file to the build:

```cmake
add_library(CheckpointPass MODULE
    src/IRBasedEstimator.cpp
    src/AssemblyBasedEstimator.cpp  # Add this line
    # ... other files
)
```

### Step 4: Use Your Estimator

Set the `estimator_type` field in your configuration file:

```json
{
  "estimator_type": "assembly",
  "energy_parameters": {
    "capacity": 100.0,
    // ... your estimator's parameters
  }
}
```

The factory will automatically use your custom estimator when processing the configuration.

### EnergyEstimator Interface

```cpp
class EnergyEstimator {
public:
    virtual ~EnergyEstimator() = default;

    /// Estimate energy for a basic block
    virtual EnergyEstimate estimate(const llvm::BasicBlock &BB) = 0;

    /// Get configured energy capacity
    virtual double getCapacity() const = 0;

    /// Get estimator name for diagnostics
    virtual std::string getName() const = 0;

    /// Optional: called before processing a function
    virtual void prepareForFunction(const llvm::Function &F) {}

    /// Optional: called after processing a function
    virtual void finalizeFunction(const llvm::Function &F) {}
};
```

## Running Tests

```bash
./tests/run_tests.sh
```

## License

MIT License

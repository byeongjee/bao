# Checkpoint Insertion for LLVM IR

This repository provides an LLVM pass plugin for checkpoint insertion under intermittent-power constraints.

The top-level hierarchy is:
- checkpoint insertion interface pass (`checkpoint-insert`),
- multiple algorithms under that interface (`milp`, `rockclimb`, and future additions).

`milp` is our main algorithm and uses a separate MILP parameter config.
`rockclimb` is kept as the baseline implementation.

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
│       ├── CheckpointInsertPass.h      # Interface/dispatcher pass
│       ├── CheckpointInsertionAlgorithm.h
│       ├── MILPNextPass.h              # `milp` algorithm pass
│       ├── MILPValidatePass.h          # Post-pass energy validator
│       └── RockClimbPass.h             # `rockclimb` baseline pass
│   └── src/
│       ├── MILPPipeline.cpp            # pass registration
│       ├── CheckpointInsertPass.cpp
│       ├── CheckpointInsertionAlgorithm.cpp
│       ├── MILPNextPass.cpp
│       ├── MILPValidatePass.cpp
│       └── RockClimbPass.cpp
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

```bash
clang -S -emit-llvm -O0 -Xclang -disable-O0-optnone input.c -o input.ll
```

> **Note:** The `-Xclang -disable-O0-optnone` flag is required to allow optimization passes to run on `-O0` compiled code.

### 2. Run via Interface Dispatcher

```bash
opt -load-pass-plugin=./passes/build/CheckpointPass.so \
    -passes=checkpoint-insert \
    -checkpoint-algorithm=milp \
    -energy-config=./benchmarks/sample_ir_energy_config.json \
    -milp-config=./benchmarks/sample_milp_config.json \
    -S input.ll -o instrumented.ll
```

### 3. Run Algorithm Directly

MILP:
```bash
opt -load-pass-plugin=./passes/build/CheckpointPass.so \
    -passes=milp \
    -energy-config=./benchmarks/sample_ir_energy_config.json \
    -milp-config=./benchmarks/sample_milp_config.json \
    -S input.ll -o instrumented.ll
```

RockClimb:
```bash
opt -load-pass-plugin=./passes/build/CheckpointPass.so \
    -passes=rockclimb \
    -energy-config=./tests/rockclimb_config.json \
    -S input.ll -o instrumented.ll
```

### 4. Run Post-pass Validation (MILP)

```bash
opt -load-pass-plugin=./passes/build/CheckpointPass.so \
    -passes=milp,milp-validate \
    -energy-config=./benchmarks/sample_ir_energy_config.json \
    -milp-config=./benchmarks/sample_milp_config.json \
    -S input.ll -o /dev/null
```

### Command-line Options (Core)

| Option | Description | Default |
|--------|-------------|---------|
| `-passes=checkpoint-insert` | Interface dispatcher pass | n/a |
| `-checkpoint-algorithm=<name>` | Algorithm for dispatcher (`milp` or `rockclimb`) | `milp` |
| `-passes=milp` | Run MILP algorithm directly | n/a |
| `-passes=rockclimb` | Run RockClimb algorithm directly | n/a |
| `-passes=milp-validate` | Validate MILP region energy safety metadata | n/a |
| `-energy-config=<path>` | Energy-estimator config path | required for all algorithms |
| `-milp-config=<path>` | MILP+validator config path | required for `milp` and `milp-validate` |

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

# Run dispatcher + milp
opt -load-pass-plugin=./passes/build/CheckpointPass.so \
    -passes=checkpoint-insert,milp-validate \
    -checkpoint-algorithm=milp \
    -energy-config=./benchmarks/sample_ir_energy_config.json \
    -milp-config=./benchmarks/sample_milp_config.json \
    -S test.ll -o /dev/null
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

Interface dispatcher smoke tests:

```bash
./tests/run_checkpoint_insert_smoke_tests.sh
```

MILP smoke tests:

```bash
./tests/run_milp_next_smoke_tests.sh
```

MILP validation smoke tests:

```bash
./tests/run_milp_validate_smoke_tests.sh
```

RockClimb baseline tests:

```bash
./tests/run_rockclimb_tests.sh
./tests/run_memory_ckpt_tests.sh
```

## License

MIT License

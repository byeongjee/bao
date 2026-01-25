#include "IRBasedEstimator.h"

#include "llvm/IR/Instructions.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/ADT/Twine.h"

#define JSON_NOEXCEPTION
#include <nlohmann/json.hpp>
#include <fstream>

namespace checkpoint {

IRBasedEstimator::IRBasedEstimator(const std::string &configPath) {
    loadConfig(configPath);
}

void IRBasedEstimator::loadConfig(const std::string &path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        llvm::report_fatal_error(llvm::Twine("Cannot open energy config file: ") + path);
    }

    nlohmann::json config = nlohmann::json::parse(file, nullptr, false);
    if (config.is_discarded()) {
        llvm::report_fatal_error(llvm::Twine("JSON parse error in: ") + path);
    }

    // Validate required fields
    if (!config.contains("energy_parameters")) {
        llvm::report_fatal_error(llvm::Twine("Missing 'energy_parameters' in config: ") + path);
    }

    auto &params = config["energy_parameters"];

    // Load capacity (required)
    if (!params.contains("capacity")) {
        llvm::report_fatal_error(llvm::Twine("Missing 'capacity' in config: ") + path);
    }
    capacity_ = params["capacity"].get<double>();

    // Load instruction costs (required)
    if (!params.contains("instruction_costs")) {
        llvm::report_fatal_error(llvm::Twine("Missing 'instruction_costs' in config: ") + path);
    }

    auto &costs = params["instruction_costs"];

    // Required cost categories
    const std::vector<std::string> requiredCategories = {
        "simple_arithmetic", "complex_arithmetic", "floating_point",
        "load", "store", "control_flow", "comparison", "conversion",
        "call", "phi_select", "gep", "alloca", "atomic", "default"
    };

    for (const auto &cat : requiredCategories) {
        if (!costs.contains(cat)) {
            llvm::report_fatal_error(llvm::Twine("Missing instruction cost category '") +
                                     cat + "' in config: " + path);
        }
        instructionCosts_[cat] = costs[cat].get<int>();
    }
}

double IRBasedEstimator::getCapacity() const {
    return capacity_;
}

std::string IRBasedEstimator::getName() const {
    return "ir-based";
}

std::string IRBasedEstimator::getCostCategory(unsigned Opcode) const {
    using namespace llvm;

    switch (Opcode) {
    // Simple arithmetic
    case Instruction::Add:
    case Instruction::Sub:
    case Instruction::And:
    case Instruction::Or:
    case Instruction::Xor:
    case Instruction::Shl:
    case Instruction::LShr:
    case Instruction::AShr:
        return "simple_arithmetic";

    // Complex arithmetic
    case Instruction::Mul:
    case Instruction::SDiv:
    case Instruction::UDiv:
    case Instruction::SRem:
    case Instruction::URem:
        return "complex_arithmetic";

    // Floating point
    case Instruction::FAdd:
    case Instruction::FSub:
    case Instruction::FMul:
    case Instruction::FDiv:
    case Instruction::FRem:
        return "floating_point";

    // Memory operations
    case Instruction::Load:
        return "load";
    case Instruction::Store:
        return "store";

    // Control flow
    case Instruction::Br:
    case Instruction::Switch:
    case Instruction::Ret:
    case Instruction::IndirectBr:
        return "control_flow";

    // Comparison
    case Instruction::ICmp:
    case Instruction::FCmp:
        return "comparison";

    // Conversions
    case Instruction::Trunc:
    case Instruction::ZExt:
    case Instruction::SExt:
    case Instruction::FPToUI:
    case Instruction::FPToSI:
    case Instruction::UIToFP:
    case Instruction::SIToFP:
    case Instruction::FPTrunc:
    case Instruction::FPExt:
    case Instruction::PtrToInt:
    case Instruction::IntToPtr:
    case Instruction::BitCast:
    case Instruction::AddrSpaceCast:
        return "conversion";

    // Function call
    case Instruction::Call:
    case Instruction::Invoke:
        return "call";

    // PHI and Select
    case Instruction::PHI:
    case Instruction::Select:
        return "phi_select";

    // GEP
    case Instruction::GetElementPtr:
        return "gep";

    // Alloca
    case Instruction::Alloca:
        return "alloca";

    // Atomics
    case Instruction::AtomicRMW:
    case Instruction::AtomicCmpXchg:
    case Instruction::Fence:
        return "atomic";

    // Default
    default:
        return "default";
    }
}

int IRBasedEstimator::getInstructionCost(unsigned Opcode) const {
    std::string category = getCostCategory(Opcode);
    auto it = instructionCosts_.find(category);
    if (it != instructionCosts_.end()) {
        return it->second;
    }
    // Fallback to default category
    auto defIt = instructionCosts_.find("default");
    if (defIt != instructionCosts_.end()) {
        return defIt->second;
    }
    return 1; // Ultimate fallback
}

EnergyEstimate IRBasedEstimator::estimate(const llvm::BasicBlock &BB) {
    double totalCost = 0.0;
    for (const llvm::Instruction &I : BB) {
        totalCost += getInstructionCost(I.getOpcode());
    }
    return EnergyEstimate{totalCost, "ir-instruction-sum"};
}

} // namespace checkpoint

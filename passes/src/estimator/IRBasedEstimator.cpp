#include "estimator/IRBasedEstimator.h"

#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

#define JSON_NOEXCEPTION
#include <nlohmann/json.hpp>
#include <fstream>

namespace checkpoint {

std::unique_ptr<IRBasedEstimator> IRBasedEstimator::create(const std::string &configPath) {
    auto estimator = std::unique_ptr<IRBasedEstimator>(new IRBasedEstimator());
    if (!estimator->loadConfig(configPath)) {
        return nullptr;
    }
    return estimator;
}

bool IRBasedEstimator::loadConfig(const std::string &path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        llvm::errs() << "Error: Cannot open energy config file: " << path << "\n";
        return false;
    }

    nlohmann::json config = nlohmann::json::parse(file, nullptr, false);
    if (config.is_discarded()) {
        llvm::errs() << "Error: JSON parse error in: " << path << "\n";
        return false;
    }

    // Validate required fields
    if (!config.contains("energy_parameters")) {
        llvm::errs() << "Error: Missing 'energy_parameters' in config: " << path << "\n";
        return false;
    }

    auto &params = config["energy_parameters"];

    // Load instruction costs (required)
    if (!params.contains("instruction_costs")) {
        llvm::errs() << "Error: Missing 'instruction_costs' in config: " << path << "\n";
        return false;
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
            llvm::errs() << "Error: Missing instruction cost category '" << cat
                         << "' in config: " << path << "\n";
            return false;
        }
        instructionCosts_[cat] = costs[cat].get<double>();
    }

    return true;
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

double IRBasedEstimator::getInstructionCost(unsigned Opcode) const {
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

double IRBasedEstimator::getInstructionCost(const llvm::Instruction &I) {
    return getInstructionCost(I.getOpcode());
}

EnergyEstimate IRBasedEstimator::estimate(const llvm::BasicBlock &BB) {
    double totalCost = 0.0;
    for (const llvm::Instruction &I : BB) {
        totalCost += getInstructionCost(I);
    }
    return EnergyEstimate{totalCost, "ir-instruction-sum"};
}

} // namespace checkpoint

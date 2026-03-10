#include "RockClimbMachinePass.h"
#include "MachineDistributedCheckpointing.h"
#include "MachineEnergyEstimator.h"
#include "RockClimbMachineInstrumenter.h"
#include "RockClimbMachineOptimizer.h"

#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <fstream>
#include <set>
#include <sstream>

using namespace llvm;

// CLI options for the machine-level RockClimb pass
static cl::opt<std::string> RockClimbMachineConfigOpt("rockclimb-machine-config",
                                                      cl::desc("Path to RockClimb config JSON"),
                                                      cl::value_desc("filename"), cl::init(""));

static cl::opt<std::string>
    RockClimbMachineEnergyConfigOpt("rockclimb-machine-energy-config",
                                    cl::desc("Path to assembly energy config JSON"),
                                    cl::value_desc("filename"), cl::init(""));

static cl::opt<std::string> RockClimbMachineEnergyDataOpt(
    "rockclimb-machine-energy-data",
    cl::desc("Pre-computed per-BB energy JSON (from bb-energy-analyzer)"),
    cl::value_desc("filename"), cl::init(""));

static cl::opt<bool>
    RockClimbMachineDumpEnergyKeysOpt("rockclimb-machine-dump-energy-keys",
                                      cl::desc("Print all required energy parameter keys and exit"),
                                      cl::init(false));

static cl::opt<bool> AddDebugMarkersOpt("add-debug-markers",
                                        cl::desc("Emit runtime function calls for register "
                                                 "save/restore (for mock counter debugging)"),
                                        cl::init(false));

namespace checkpoint {

/// Parameters parsed from the rockclimb machine config JSON.
/// Mirrors RockClimbParams from the IR-level pass.
struct MachineRockClimbParams {
    double capacity = 0.0;
    unsigned N_reg = 0;
    double reg_restore_energy = 0.0;
    bool distributedCheckpointing = true;
    double checkpoint_store_energy = 0.0;
    bool addDebugMarkers = false;

    double calculateESafe() const { return capacity - N_reg * reg_restore_energy; }
};

/// Parse rockclimb config from JSON. Same format as IR-level pass.
static bool parseMachineRockClimbParams(StringRef configPath, MachineRockClimbParams &params) {
    std::ifstream file(configPath.str());
    if (!file.is_open()) {
        errs() << "Error: Cannot open RockClimb machine config: " << configPath << "\n";
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    Expected<json::Value> parsed = json::parse(content);
    if (!parsed) {
        consumeError(parsed.takeError());
        errs() << "Error: JSON parse error in config: " << configPath << "\n";
        return false;
    }

    json::Object *root = parsed->getAsObject();
    if (!root) {
        errs() << "Error: Config is not a JSON object: " << configPath << "\n";
        return false;
    }

    // Read capacity (or legacy E_input)
    auto capacity = root->getNumber("capacity");
    if (!capacity) {
        auto E_input = root->getNumber("E_input");
        if (!E_input) {
            errs() << "Error: Missing 'capacity' in config\n";
            return false;
        }
        capacity = E_input;
    }

    auto N_reg = root->getInteger("N_reg");
    if (!N_reg) {
        errs() << "Error: Missing 'N_reg' in config\n";
        return false;
    }

    auto reg_restore_energy = root->getNumber("reg_restore_energy");
    if (!reg_restore_energy) {
        errs() << "Error: Missing 'reg_restore_energy' in config\n";
        return false;
    }

    params.capacity = *capacity;
    params.N_reg = static_cast<unsigned>(*N_reg);
    params.reg_restore_energy = *reg_restore_energy;

    // distributed_checkpointing: check rockclimb section first, then root
    json::Object *rcSection = nullptr;
    if (auto *rcVal = root->get("rockclimb"))
        rcSection = rcVal->getAsObject();

    std::optional<bool> distributed;
    if (rcSection) {
        if (auto val = rcSection->getBoolean("distributed_checkpointing"))
            distributed = val;
    }
    if (!distributed) {
        if (auto val = root->getBoolean("distributed_checkpointing"))
            distributed = val;
    }
    params.distributedCheckpointing = distributed.value_or(true);

    // checkpoint_store_energy (optional)
    if (rcSection) {
        if (auto v = rcSection->getNumber("checkpoint_store_energy"))
            params.checkpoint_store_energy = *v;
    }
    if (params.checkpoint_store_energy == 0.0) {
        if (auto v = root->getNumber("checkpoint_store_energy"))
            params.checkpoint_store_energy = *v;
    }

    // add_debug_markers (optional, default false)
    if (auto val = root->getBoolean("add_debug_markers"))
        params.addDebugMarkers = *val;

    return true;
}

char RockClimbMachinePass::ID = 0;

RockClimbMachinePass::RockClimbMachinePass() : MachineFunctionPass(ID) {}

bool RockClimbMachinePass::runOnMachineFunction(MachineFunction &MF) {
    const auto totalStart = std::chrono::steady_clock::now();

    // Validate config paths
    if (RockClimbMachineConfigOpt.empty()) {
        errs() << "Error: -rockclimb-machine-config not specified\n";
        return false;
    }
    bool hasPrecomputed = !RockClimbMachineEnergyDataOpt.empty();
    if (!hasPrecomputed && RockClimbMachineEnergyConfigOpt.empty()) {
        errs() << "Error: Either -rockclimb-machine-energy-data or "
                  "-rockclimb-machine-energy-config must be specified\n";
        return false;
    }

    // Parse config
    MachineRockClimbParams params;
    if (!parseMachineRockClimbParams(RockClimbMachineConfigOpt, params))
        return false;

    double E_safe = params.calculateESafe();

    errs() << "=== RockClimb Machine Pass on " << MF.getName() << " ===\n";
    errs() << "  Capacity: " << params.capacity << "\n";
    errs() << "  E_safe: " << E_safe << "\n";
    errs() << "  N_reg: " << params.N_reg << "\n";
    errs() << "  Distributed checkpointing: "
           << (params.distributedCheckpointing ? "enabled" : "disabled") << "\n";
    errs() << "  Energy estimation: "
           << (hasPrecomputed ? "pre-computed (bb-energy-analyzer)" : "MIR instruction-level")
           << "\n";
    errs() << "  Basic blocks: " << MF.size() << "\n";

    // Create energy estimator
    std::unique_ptr<MachineEnergyEstimator> estimatorOwned;
    MachineEnergyEstimator *estimatorPtr = nullptr;

    if (hasPrecomputed) {
        estimatorOwned =
            MachineEnergyEstimator::fromPrecomputed(RockClimbMachineEnergyDataOpt.getValue());
        if (!estimatorOwned) {
            errs() << "Error: Failed to load pre-computed energy data\n";
            return false;
        }
        estimatorPtr = estimatorOwned.get();
    } else {
        estimatorOwned =
            std::make_unique<MachineEnergyEstimator>(RockClimbMachineEnergyConfigOpt.getValue());
        estimatorPtr = estimatorOwned.get();
    }

    MachineEnergyEstimator &estimator = *estimatorPtr;

    // Dump all required energy keys if requested (only for instruction-level mode)
    if (RockClimbMachineDumpEnergyKeysOpt) {
        if (estimator.isPrecomputed()) {
            errs() << "Note: --dump-energy-keys has no effect with pre-computed energy data\n";
            return false;
        }

        std::set<std::string> allKeys, missingKeys;
        estimator.collectRequiredKeys(MF, allKeys, missingKeys);

        errs() << "\n=== Required Energy Parameter Keys (" << MF.getName() << ") ===\n";
        for (const auto &key : allKeys) {
            bool missing = missingKeys.count(key);
            errs() << "  " << key;
            if (missing)
                errs() << "  [MISSING]";
            errs() << "\n";
        }
        errs() << "\nTotal: " << allKeys.size() << " keys, " << missingKeys.size()
               << " missing\n\n";

        if (!missingKeys.empty()) {
            errs() << "Missing keys as JSON snippet:\n";
            errs() << "{\n";
            bool first = true;
            for (const auto &key : missingKeys) {
                if (!first)
                    errs() << ",\n";
                errs() << "  \"" << key << "\": 0.0";
                first = false;
            }
            errs() << "\n}\n\n";
        }

        return false;
    }

    // Get MachineLoopInfo
    auto &MLI = getAnalysis<MachineLoopInfoWrapperPass>().getLI();

    // Algorithm 1: region partitioning
    RockClimbMachineOptimizer optimizer(MF, MLI, estimator, E_safe);

    // If checkpoint_store_energy > 0 and distributed checkpointing enabled,
    // do preliminary partition → compute extra costs → re-run
    if (params.checkpoint_store_energy > 0 && params.distributedCheckpointing) {
        auto prelimResult = optimizer.optimize();
        if (!prelimResult.feasible) {
            errs() << "Region partitioning failed: " << prelimResult.errorMessage << "\n";
            return false;
        }

        MachineDistributedCheckpointing prelimCkpt(prelimResult.regions, MF);
        auto prelimPoints = prelimCkpt.analyze();

        // Compute per-block extra energy from checkpoint stores
        DenseMap<MachineBasicBlock *, double> ckptCosts;
        for (const auto &ckpt : prelimPoints) {
            if (ckpt.afterInst)
                ckptCosts[ckpt.afterInst->getParent()] += params.checkpoint_store_energy;
        }
        optimizer.setExtraBlockCosts(ckptCosts);
    }

    MachineRockClimbResult result = optimizer.optimize();
    if (!result.feasible) {
        errs() << "Region partitioning failed: " << result.errorMessage << "\n";
        return false;
    }

    // Distributed checkpointing analysis
    std::vector<MachineCheckpointPoint> checkpointPoints;
    if (params.distributedCheckpointing) {
        MachineDistributedCheckpointing distCkpt(result.regions, MF);
        checkpointPoints = distCkpt.analyze();
    }

    // Resolve addDebugMarkers from CLI option or config
    bool addDebugMarkers = AddDebugMarkersOpt.getValue() || params.addDebugMarkers;

    // Get or create __nvm_regs as an external global: [16 x i16]
    Module *M = const_cast<Module *>(MF.getFunction().getParent());
    GlobalVariable *nvmRegsGV = M->getGlobalVariable("__nvm_regs");
    if (!nvmRegsGV) {
        auto *i16Ty = Type::getInt16Ty(M->getContext());
        auto *arrayTy = ArrayType::get(i16Ty, 16);
        nvmRegsGV =
            new GlobalVariable(*M, arrayTy, /*isConstant=*/false, GlobalValue::ExternalLinkage,
                               /*Initializer=*/nullptr, "__nvm_regs");
    }

    // Instrumentation
    RockClimbMachineInstrumenter instrumenter(MF, addDebugMarkers, nvmRegsGV);
    unsigned insertedCount = instrumenter.instrument(result.regionBoundaries, checkpointPoints,
                                                     params.distributedCheckpointing);

    const auto totalEnd = std::chrono::steady_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();

    // Print statistics
    errs() << "\n=== Checkpoint Insertion Statistics ===\n";
    errs() << "  Pass:                            RockClimb-Machine\n";
    errs() << "  Function:                        " << MF.getName() << "\n";
    errs() << "  Basic blocks:                    " << MF.size() << "\n";
    errs() << "  Regions:                         " << result.regions.size() << "\n";
    errs() << "  Region boundaries:               " << result.regionBoundaries.size() << "\n";
    errs() << "  --- RockClimb-Machine-specific ---\n";
    // Boundary checks = boundaries - 1 (entry block skipped)
    unsigned boundaryChecks = result.regionBoundaries.empty()
                                  ? 0
                                  : static_cast<unsigned>(result.regionBoundaries.size()) - 1;
    errs() << "  Boundary checks:                 " << boundaryChecks << "\n";
    errs() << "  Register checkpoints:            " << checkpointPoints.size() << "\n";
    errs() << "  Total instrumentation points:    " << insertedCount << "\n";
    errs() << "  Compilation time (ms):           " << totalMs << "\n";

    return insertedCount > 0; // Return true if we modified the function
}

void RockClimbMachinePass::getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<MachineLoopInfoWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
}

} // namespace checkpoint

// Forward-declare the generated initialization function in the llvm namespace
namespace llvm {
void initializeRockClimbMachinePassPass(PassRegistry &);
} // namespace llvm

// INITIALIZE_PASS macros require unqualified class name — use a type alias
using RockClimbMachinePass = checkpoint::RockClimbMachinePass;

INITIALIZE_PASS_BEGIN(RockClimbMachinePass, "rockclimb-machine",
                      "RockClimb Machine-Level Checkpoint Insertion", false, false)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfoWrapperPass)
INITIALIZE_PASS_END(RockClimbMachinePass, "rockclimb-machine",
                    "RockClimb Machine-Level Checkpoint Insertion", false, false)

// Auto-registration via static constructor when .so is loaded by llc -load
namespace {
struct StaticInitializer {
    StaticInitializer() { initializeRockClimbMachinePassPass(*PassRegistry::getPassRegistry()); }
} X;
} // namespace

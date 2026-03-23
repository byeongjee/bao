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

#include "common/BlockUtils.h"
#include "common/Logger.h"
#include "common/PassStatistics.h"

#include <chrono>
#include <fstream>
#include <set>
#include <sstream>

using namespace llvm;

// CLI options for the machine-level RockClimb pass
static cl::opt<std::string> RockClimbMachineConfigOpt("rockclimb-config",
                                                      cl::desc("Path to RockClimb config JSON"),
                                                      cl::value_desc("filename"), cl::init(""));

static cl::opt<std::string>
    RockClimbMachineEnergyConfigOpt("rockclimb-energy-config",
                                    cl::desc("Path to assembly energy config JSON"),
                                    cl::value_desc("filename"), cl::init(""));

static cl::opt<std::string> RockClimbMachineEnergyDataOpt(
    "rockclimb-energy-data", cl::desc("Pre-computed per-BB energy JSON (from bb-energy-analyzer)"),
    cl::value_desc("filename"), cl::init(""));

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
    double reg_store_energy = 0.0; // required, parsed from root
    bool addDebugMarkers = false;

    double calculateESafe() const { return capacity - N_reg * reg_restore_energy; }
};

/// Parse rockclimb config from JSON. Same format as IR-level pass.
static bool parseMachineRockClimbParams(StringRef configPath, MachineRockClimbParams &params) {
    std::ifstream file(configPath.str());
    if (!file.is_open()) {
        PLOGE << "Error: Cannot open RockClimb machine config: " << configPath;
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    Expected<json::Value> parsed = json::parse(content);
    if (!parsed) {
        consumeError(parsed.takeError());
        PLOGE << "Error: JSON parse error in config: " << configPath;
        return false;
    }

    json::Object *root = parsed->getAsObject();
    if (!root) {
        PLOGE << "Error: Config is not a JSON object: " << configPath;
        return false;
    }

    // Read capacity (or legacy E_input)
    auto capacity = root->getNumber("capacity");
    if (!capacity) {
        auto E_input = root->getNumber("E_input");
        if (!E_input) {
            PLOGE << "Error: Missing 'capacity' in config";
            return false;
        }
        capacity = E_input;
    }

    auto N_reg = root->getInteger("N_reg");
    if (!N_reg) {
        PLOGE << "Error: Missing 'N_reg' in config";
        return false;
    }

    auto reg_store_energy = root->getNumber("reg_store_energy");
    if (!reg_store_energy) {
        PLOGE << "Error: Missing 'reg_store_energy' in config";
        return false;
    }

    auto reg_restore_energy = root->getNumber("reg_restore_energy");
    if (!reg_restore_energy) {
        PLOGE << "Error: Missing 'reg_restore_energy' in config";
        return false;
    }

    params.capacity = *capacity;
    params.N_reg = static_cast<unsigned>(*N_reg);
    params.reg_store_energy = *reg_store_energy;
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

    // add_debug_markers (optional, default false)
    if (auto val = root->getBoolean("add_debug_markers"))
        params.addDebugMarkers = *val;

    return true;
}

/// Build a CommonStats struct for RockClimb, computing edge count and timing.
static CommonStats buildRockClimbStats(MachineFunction &MF,
                                       std::chrono::steady_clock::time_point totalStart) {
    const auto totalEnd = std::chrono::steady_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();
    unsigned edgeCount = 0;
    for (auto &MBB : MF)
        edgeCount += MBB.succ_size();
    CommonStats c;
    c.passName = "RockClimb-Machine";
    c.functionName = MF.getName().str();
    c.basicBlocks = MF.size();
    c.edges = edgeCount;
    c.compilationTimeMs = totalMs;
    c.peakRSSKb = getPeakRSSKb();
    return c;
}

/// Write an infeasible-result JSON sidecar for RockClimb.
static void writeInfeasibleJSON(MachineFunction &MF,
                                std::chrono::steady_clock::time_point totalStart,
                                const std::string &reason) {
    if (StatsJsonOpt.empty())
        return;
    auto c = buildRockClimbStats(MF, totalStart);
    auto root = commonStatsToJSON(c);
    root["feasible"] = false;
    root["infeasibility_reason"] = reason;
    writeStatsJSON(StatsJsonOpt, std::move(root));
}

char RockClimbMachinePass::ID = 0;

RockClimbMachinePass::RockClimbMachinePass() : MachineFunctionPass(ID) {}

// Runtime/instrumentation functions that must not be checkpointed.
static bool isRuntimeFunction(StringRef name) {
    static const char *const skip[] = {
        "timing_gpio_init", "timing_gpio_start", "timing_gpio_stop", "_timing_delay_cycles",
        "debug_init",       "debug_exit",        "uart_init",        "uart_putc",
        "uart_puts",        "uart_put_u16",
    };
    for (const char *s : skip)
        if (name == s)
            return true;
    return false;
}

bool RockClimbMachinePass::runOnMachineFunction(MachineFunction &MF) {
    checkpoint::initLogging();
    const auto totalStart = std::chrono::steady_clock::now();

    if (isRuntimeFunction(MF.getName()))
        return false;

    // Validate config paths
    if (RockClimbMachineConfigOpt.empty()) {
        PLOGE << "Error: -rockclimb-config not specified";
        return false;
    }
    bool hasPrecomputed = !RockClimbMachineEnergyDataOpt.empty();
    if (!hasPrecomputed && RockClimbMachineEnergyConfigOpt.empty()) {
        PLOGE << "Error: Either -rockclimb-energy-data or "
                 "-rockclimb-energy-config must be specified";
        return false;
    }

    // Parse config
    MachineRockClimbParams params;
    if (!parseMachineRockClimbParams(RockClimbMachineConfigOpt, params))
        return false;

    double E_safe = params.calculateESafe();

    PLOGI << "=== RockClimb Machine Pass on " << MF.getName() << " ===";
    PLOGI << "  Capacity: " << params.capacity;
    PLOGI << "  E_safe: " << E_safe;
    PLOGI << "  N_reg: " << params.N_reg;
    PLOGI << "  Distributed checkpointing: "
          << (params.distributedCheckpointing ? "enabled" : "disabled");
    PLOGI << "  Energy estimation: "
          << (hasPrecomputed ? "pre-computed (bb-energy-analyzer)" : "MIR instruction-level");
    PLOGI << "  Basic blocks: " << MF.size();

    // Create energy estimator
    std::unique_ptr<MachineEnergyEstimator> estimatorOwned;
    MachineEnergyEstimator *estimatorPtr = nullptr;

    if (hasPrecomputed) {
        estimatorOwned =
            MachineEnergyEstimator::fromPrecomputed(RockClimbMachineEnergyDataOpt.getValue());
        if (!estimatorOwned) {
            PLOGE << "Error: Failed to load pre-computed energy data";
            return false;
        }
        estimatorPtr = estimatorOwned.get();
    } else {
        estimatorOwned =
            std::make_unique<MachineEnergyEstimator>(RockClimbMachineEnergyConfigOpt.getValue());
        estimatorPtr = estimatorOwned.get();
    }

    MachineEnergyEstimator &estimator = *estimatorPtr;

    // Report required/missing energy keys for MIR estimation mode
    std::set<std::string> requiredEnergyKeys, missingEnergyKeys;
    if (!hasPrecomputed) {
        estimator.collectRequiredKeys(MF, requiredEnergyKeys, missingEnergyKeys);

        {
            std::string reqList;
            bool first = true;
            for (const auto &key : requiredEnergyKeys) {
                reqList += (first ? " " : ", ") + key;
                first = false;
            }
            PLOGI << "--- Energy parameters for " << MF.getName() << " ---";
            PLOGI << "  Required (" << requiredEnergyKeys.size() << " keys):" << reqList;
        }
        {
            std::string missList;
            bool first = true;
            for (const auto &key : missingEnergyKeys) {
                missList += (first ? " " : ", ") + key;
                first = false;
            }
            PLOGI << "  Missing  (" << missingEnergyKeys.size() << " keys):" << missList;
        }
    }

    // Get MachineLoopInfo
    auto &MLI = getAnalysis<MachineLoopInfoWrapperPass>().getLI();

    // Algorithm 1: region partitioning
    double regStoreEnergy = params.distributedCheckpointing ? params.reg_store_energy : 0.0;
    RockClimbMachineOptimizer optimizer(MF, MLI, estimator, E_safe, regStoreEnergy);

    MachineRockClimbResult result = optimizer.optimize();
    if (!result.feasible) {
        PLOGE << "Region partitioning failed: " << result.errorMessage;
        writeInfeasibleJSON(MF, totalStart, result.errorMessage);
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

    // Get or create debug counter globals (uint16_t in .nvm section)
    GlobalVariable *cntBoundaryGV = nullptr;
    GlobalVariable *cntSaveGV = nullptr;
    GlobalVariable *cntRestoreGV = nullptr;
    if (addDebugMarkers) {
        auto *i16Ty = Type::getInt16Ty(M->getContext());
        auto getOrCreateCounter = [&](const char *name) -> GlobalVariable * {
            GlobalVariable *gv = M->getGlobalVariable(name);
            if (!gv) {
                gv = new GlobalVariable(*M, i16Ty, /*isConstant=*/false,
                                        GlobalValue::ExternalLinkage,
                                        /*Initializer=*/nullptr, name);
            }
            return gv;
        };
        cntBoundaryGV = getOrCreateCounter("cnt_boundary");
        cntSaveGV = getOrCreateCounter("cnt_save_reg");
        cntRestoreGV = getOrCreateCounter("cnt_restore_reg");
    }

    // Instrumentation
    RockClimbMachineInstrumenter instrumenter(MF, addDebugMarkers, nvmRegsGV, cntBoundaryGV,
                                              cntSaveGV, cntRestoreGV);
    unsigned insertedCount = instrumenter.instrument(result.regionBoundaries, checkpointPoints,
                                                     params.distributedCheckpointing);

    const auto totalEnd = std::chrono::steady_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();

    // Compute edge count
    unsigned edgeCount = 0;
    for (auto &MBB : MF)
        edgeCount += MBB.succ_size();

    // Print statistics
    // Boundary checks = boundaries - 1 (entry block skipped)
    unsigned boundaryChecks = result.regionBoundaries.empty()
                                  ? 0
                                  : static_cast<unsigned>(result.regionBoundaries.size()) - 1;
    PLOGI << "=== Checkpoint Insertion Statistics ===";
    PLOGI << "  Pass:                            RockClimb-Machine";
    PLOGI << "  Function:                        " << MF.getName();
    PLOGI << "  Basic blocks:                    " << MF.size();
    PLOGI << "  Edges:                           " << edgeCount;
    PLOGI << "  Regions:                         " << result.regions.size();
    PLOGI << "  Region boundaries:               " << result.regionBoundaries.size();
    PLOGI << "  --- RockClimb-Machine-specific ---";
    PLOGI << "  Boundary checks:                 " << boundaryChecks;
    PLOGI << "  Register checkpoints:            " << checkpointPoints.size();
    PLOGI << "  Total instrumentation points:    " << insertedCount;
    PLOGI << "  Compilation time (ms):           " << totalMs;
    PLOGI << "  Peak RSS (KB):                   " << checkpoint::getPeakRSSKb();

    if (!StatsJsonOpt.empty()) {
        CommonStats c;
        c.passName = "RockClimb-Machine";
        c.functionName = MF.getName().str();
        c.basicBlocks = MF.size();
        c.edges = edgeCount;
        c.regions = result.regions.size();
        c.regionBoundaries = result.regionBoundaries.size();
        c.compilationTimeMs = totalMs;
        c.peakRSSKb = getPeakRSSKb();
        auto root = commonStatsToJSON(c);
        root["boundary_checks"] = static_cast<int64_t>(boundaryChecks);
        root["feasible"] = true;
        if (!requiredEnergyKeys.empty()) {
            json::Array reqArr, missArr;
            for (const auto &k : requiredEnergyKeys)
                reqArr.push_back(k);
            for (const auto &k : missingEnergyKeys)
                missArr.push_back(k);
            root["required_energy_keys"] = std::move(reqArr);
            root["missing_energy_keys"] = std::move(missArr);
        }
        writeStatsJSON(StatsJsonOpt, std::move(root));
    }

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

INITIALIZE_PASS_BEGIN(RockClimbMachinePass, "rockclimb",
                      "RockClimb Machine-Level Checkpoint Insertion", false, false)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfoWrapperPass)
INITIALIZE_PASS_END(RockClimbMachinePass, "rockclimb",
                    "RockClimb Machine-Level Checkpoint Insertion", false, false)

// Auto-registration via static constructor when .so is loaded by llc -load
namespace {
struct StaticInitializer {
    StaticInitializer() { initializeRockClimbMachinePassPass(*PassRegistry::getPassRegistry()); }
} X;
} // namespace

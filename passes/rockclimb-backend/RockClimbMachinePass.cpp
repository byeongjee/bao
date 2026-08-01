#include "RockClimbMachinePass.h"
#include "MachineDistributedCheckpointing.h"
#include "MachineEnergyEstimator.h"
#include "RockClimbMachineInstrumenter.h"
#include "RockClimbMachineOptimizer.h"
#include "common/RockClimbConfig.h"

#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/JSON.h"

#include "common/BlockUtils.h"
#include "common/Logger.h"
#include "common/PassStatistics.h"

#include <chrono>
#include <set>

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

namespace checkpoint {
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
bool isRuntimeFunction(StringRef name) {
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

    // Validate config paths. Returning false here would let llc exit 0 with an
    // uninstrumented binary, so configuration errors abort loudly instead.
    if (RockClimbMachineConfigOpt.empty())
        report_fatal_error("RockClimb: -rockclimb-config not specified",
                           /*gen_crash_diag=*/false);
    bool hasPrecomputed = !RockClimbMachineEnergyDataOpt.empty();
    if (!hasPrecomputed && RockClimbMachineEnergyConfigOpt.empty())
        report_fatal_error("RockClimb: either -rockclimb-energy-data or "
                           "-rockclimb-energy-config must be specified",
                           /*gen_crash_diag=*/false);

    // Parse config (details of the failure are logged by the parser)
    RockClimbParams params;
    if (!parseRockClimbParams(RockClimbMachineConfigOpt, params))
        report_fatal_error(Twine("RockClimb: invalid config ") + RockClimbMachineConfigOpt,
                           /*gen_crash_diag=*/false);

    double E_safe = params.calculateESafe();

    PLOGI << "=== RockClimb Machine Pass on " << MF.getName() << " ===";
    PLOGI << "  Capacity: " << params.capacity;
    PLOGI << "  E_pro: " << params.EPro;
    PLOGI << "  E_epi: " << params.EEpi;
    PLOGI << "  E_safe: " << E_safe;
    PLOGI << "  N_reg: " << params.NReg;
    PLOGI << "  Distributed checkpointing: "
          << (params.distributedCheckpointing ? "enabled" : "disabled");
    PLOGI << "  Energy estimation: "
          << (hasPrecomputed ? "pre-computed (bb-energy-analyzer)" : "MIR instruction-level");
    PLOGI << "  Basic blocks: " << MF.size();

    // A nonpositive budget means the capacitor cannot even cover the fixed
    // prologue/epilogue/restore overhead — report a clear infeasibility
    // instead of a confusing "block exceeds E_safe (negative)" for the first
    // block. ("Region partitioning failed" is the phrase the benchmark
    // driver's infeasibility detector keys on.)
    if (E_safe <= 0) {
        std::string reason = "E_safe " + std::to_string(E_safe) + " <= 0: capacity " +
                             std::to_string(params.capacity) +
                             " cannot cover E_pro + E_epi + (N_reg-2)*reg_restore_energy";
        PLOGE << "Region partitioning failed: " << reason;
        writeInfeasibleJSON(MF, totalStart, reason);
        return false;
    }

    // Create energy estimator
    std::unique_ptr<MachineEnergyEstimator> estimator;
    if (hasPrecomputed) {
        estimator =
            MachineEnergyEstimator::fromPrecomputed(RockClimbMachineEnergyDataOpt.getValue());
        if (!estimator)
            report_fatal_error(Twine("RockClimb: failed to load pre-computed energy data ") +
                                   RockClimbMachineEnergyDataOpt,
                               /*gen_crash_diag=*/false);
    } else {
        estimator =
            std::make_unique<MachineEnergyEstimator>(RockClimbMachineEnergyConfigOpt.getValue());
    }

    // Report required/missing energy keys for MIR estimation mode
    std::set<std::string> requiredEnergyKeys, missingEnergyKeys;
    if (!hasPrecomputed) {
        estimator->collectRequiredKeys(MF, requiredEnergyKeys, missingEnergyKeys);

        auto joinKeys = [](const std::set<std::string> &keys) {
            std::string list;
            bool first = true;
            for (const auto &key : keys) {
                list += (first ? " " : ", ") + key;
                first = false;
            }
            return list;
        };
        PLOGI << "--- Energy parameters for " << MF.getName() << " ---";
        PLOGI << "  Required (" << requiredEnergyKeys.size()
              << " keys):" << joinKeys(requiredEnergyKeys);
        PLOGI << "  Missing  (" << missingEnergyKeys.size()
              << " keys):" << joinKeys(missingEnergyKeys);
    }

    // Get MachineLoopInfo
    auto &MLI = getAnalysis<MachineLoopInfoWrapperPass>().getLI();

    // Algorithm 1: region partitioning
    double regStoreEnergy = params.distributedCheckpointing ? params.regStoreEnergy : 0.0;
    RockClimbMachineOptimizer optimizer(MF, MLI, *estimator, E_safe, regStoreEnergy);

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
    RockClimbMachineInstrumenter instrumenter(MF, nvmRegsGV);
    unsigned insertedCount = instrumenter.instrument(result.regionBoundaries, checkpointPoints,
                                                     params.distributedCheckpointing);

    CommonStats stats = buildRockClimbStats(MF, totalStart);
    stats.regions = result.regions.size();
    stats.regionBoundaries = result.regionBoundaries.size();

    // Boundary checks = all region boundaries (the entry boundary is now
    // emitted too, with live-in argument saves).
    unsigned boundaryChecks = static_cast<unsigned>(result.regionBoundaries.size());
    PLOGI << "=== Checkpoint Insertion Statistics ===";
    PLOGI << "  Pass:                            " << stats.passName;
    PLOGI << "  Function:                        " << stats.functionName;
    PLOGI << "  Basic blocks:                    " << stats.basicBlocks;
    PLOGI << "  Edges:                           " << stats.edges;
    PLOGI << "  Regions:                         " << stats.regions;
    PLOGI << "  Region boundaries:               " << stats.regionBoundaries;
    PLOGI << "  --- RockClimb-Machine-specific ---";
    PLOGI << "  Boundary checks:                 " << boundaryChecks;
    PLOGI << "  Register checkpoints:            " << checkpointPoints.size();
    PLOGI << "  Total instrumentation points:    " << insertedCount;
    PLOGI << "  Compilation time (ms):           " << stats.compilationTimeMs;
    PLOGI << "  Peak RSS (KB):                   " << stats.peakRSSKb;

    if (!StatsJsonOpt.empty()) {
        auto root = commonStatsToJSON(stats);
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

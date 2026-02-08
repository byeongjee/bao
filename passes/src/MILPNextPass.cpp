#include "MILPNextPass.h"

#include "BlockUtils.h"
#include "MILPContext.h"
#include "MILPModelTypes.h"
#include "MILPObjectAnalysis.h"
#include "MILPOptions.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <set>
#include <string>
#include <vector>

using namespace llvm;

namespace {

constexpr const char *RegionStartMetadataKey = "milp.region.starts";

static cl::opt<unsigned> MILPNextMaxObjectsPrinted(
    "milp-next-max-objects-printed",
    cl::desc("Maximum number of object names to print in milp-next diagnostics"),
    cl::init(12));

void printObjectSample(StringRef label,
                       const std::vector<checkpoint::MILPTrackedObject> &objects,
                       unsigned maxCount) {
    errs() << "  " << label << ": " << objects.size() << "\n";
    unsigned printed = 0;
    for (const auto &obj : objects) {
        if (printed >= maxCount) {
            errs() << "    ... (" << (objects.size() - printed) << " more)\n";
            break;
        }
        errs() << "    - " << obj.name
               << " [size=" << obj.sizeBytes
               << "B, global=" << (obj.isGlobal ? "yes" : "no")
               << "] reason: " << obj.reason << "\n";
        ++printed;
    }
}

struct BoundaryPlan {
    bool feasible = true;
    std::vector<std::string> regionStartBlocks;
    std::string errorMessage;
};

BoundaryPlan buildGreedyBoundaryPlan(const checkpoint::MILPParameters &params,
                                     llvm::Function &F,
                                     const checkpoint::CFGAnalysis &cfg) {
    BoundaryPlan plan;
    std::set<std::string> forbidden(params.forbiddenBoundaryBlocks.begin(),
                                    params.forbiddenBoundaryBlocks.end());

    const std::string entryBlock = cfg.getEntryBlock();
    if (entryBlock.empty()) {
        plan.feasible = false;
        plan.errorMessage = "Unable to identify entry block";
        return plan;
    }
    if (forbidden.count(entryBlock)) {
        plan.feasible = false;
        plan.errorMessage = "Entry block is forbidden by milp config";
        return plan;
    }

    plan.regionStartBlocks.push_back(entryBlock);
    double runningEnergy_nJ = params.regionPrologueOverhead_nJ;

    for (llvm::BasicBlock &BB : F) {
        const std::string blockName = checkpoint::getBlockName(BB, F);
        const double blockEnergy_nJ = cfg.getBlockInfo(blockName).energyCost;
        if (blockName != entryBlock &&
            runningEnergy_nJ + blockEnergy_nJ > params.energyBudget_nJ) {
            if (forbidden.count(blockName)) {
                plan.feasible = false;
                plan.errorMessage = "Required boundary block '" + blockName +
                                    "' is forbidden by milp config";
                return plan;
            }
            plan.regionStartBlocks.push_back(blockName);
            runningEnergy_nJ = params.regionPrologueOverhead_nJ;
        }

        if (runningEnergy_nJ + blockEnergy_nJ > params.energyBudget_nJ) {
            plan.feasible = false;
            plan.errorMessage = "Block '" + blockName +
                                "' exceeds region budget even as a region start";
            return plan;
        }
        runningEnergy_nJ += blockEnergy_nJ;
    }

    return plan;
}

void writeRegionStartMetadata(llvm::Function &F,
                              const std::vector<std::string> &regionStarts) {
    llvm::LLVMContext &Ctx = F.getContext();
    llvm::SmallVector<llvm::Metadata*, 16> operands;
    operands.reserve(regionStarts.size());
    for (const std::string &name : regionStarts) {
        operands.push_back(llvm::MDString::get(Ctx, name));
    }
    F.setMetadata(RegionStartMetadataKey, llvm::MDNode::get(Ctx, operands));
}

} // namespace

namespace checkpoint {

PreservedAnalyses MILPNextPass::run(Function &F, FunctionAnalysisManager &AM) {
    auto &LI = AM.getResult<LoopAnalysis>(F);
    auto contextResult = createMILPContext(
        F, LI, EnergyConfigOpt.getValue(), MILPConfigOpt.getValue(), "milp-next pass");
    if (!contextResult.success()) {
        if (!contextResult.shouldSkip()) {
            errs() << contextResult.errorMessage;
        }
        return PreservedAnalyses::all();
    }

    auto &context = *contextResult.context;
    MILPObjectAnalysis objectAnalysis(F);
    MILPObjectAnalysisResult objectResult = objectAnalysis.analyze();

    DefinitionSiteInventory defInventory = collectDefinitionSiteInventory(F);
    const size_t blockCount = context.cfg->getBlocks().size();
    const size_t estimatedLiveInObjectPairCount =
        blockCount * objectResult.vmCandidateObjects.size();
    MILPVariableInventory varInventory = buildMILPVariableInventory(
        blockCount,
        defInventory.totalDefinitionSites(),
        objectResult.vmCandidateObjects.size(),
        estimatedLiveInObjectPairCount);

    const MILPParameters &params = context.milpParameters;
    BoundaryPlan plan = buildGreedyBoundaryPlan(params, F, *context.cfg);
    if (!plan.feasible) {
        errs() << "Error: milp boundary planning failed in function '" << F.getName()
               << "': " << plan.errorMessage << "\n";
        return PreservedAnalyses::all();
    }
    writeRegionStartMetadata(F, plan.regionStartBlocks);

    errs() << "=== milp bring-up (phase-2) on " << F.getName() << " ===\n";
    errs() << "Configuration:\n";
    errs() << "  energy_budget_nJ: " << params.energyBudget_nJ << "\n";
    errs() << "  vm_capacity_bytes: " << params.vmCapacityBytes << "\n";
    errs() << "  boundary_reboot_probability: " << params.boundaryRebootProbability << "\n";
    errs() << "  default_loop_bound: " << params.defaultLoopBound << "\n";
    errs() << "  forbidden_boundary_blocks: " << params.forbiddenBoundaryBlocks.size() << "\n";

    errs() << "Definition-site inventory:\n";
    errs() << "  ssa_definition_sites: " << defInventory.ssaDefinitionSites << "\n";
    errs() << "  store_definition_sites: " << defInventory.storeDefinitionSites << "\n";
    errs() << "  memory_writing_call_definition_sites: "
           << defInventory.memoryWritingCallDefinitionSites << "\n";
    errs() << "  total_definition_sites: " << defInventory.totalDefinitionSites() << "\n";

    errs() << "MILP variable inventory (descriptive names):\n";
    errs() << "  is_region_start[*]: " << varInventory.isRegionStartCount << "\n";
    errs() << "  is_checkpoint_enabled[*]: " << varInventory.isCheckpointEnabledCount << "\n";
    errs() << "  is_object_in_vm[*]: " << varInventory.isObjectInVMCount << "\n";
    errs() << "  needs_vm_object_restore[*]: "
           << varInventory.needsVMObjectRestoreCount << "\n";
    errs() << "  accumulated_region_energy_nJ[*]: "
           << varInventory.accumulatedRegionEnergyCount << "\n";

    errs() << "Object classification (conservative):\n";
    printObjectSample("vm_candidate_objects",
                      objectResult.vmCandidateObjects,
                      MILPNextMaxObjectsPrinted);
    printObjectSample("forced_nvm_objects",
                      objectResult.forcedNVMObjects,
                      MILPNextMaxObjectsPrinted);
    printObjectSample("excluded_objects",
                      objectResult.excludedObjects,
                      MILPNextMaxObjectsPrinted);

    errs() << "Region boundaries (metadata only): " << plan.regionStartBlocks.size() << "\n";
    for (const std::string &name : plan.regionStartBlocks) {
        errs() << "  - " << name << "\n";
    }

    errs() << "Note: phase-2 milp currently writes region-start metadata only.\n";
    return PreservedAnalyses::none();
}

} // namespace checkpoint

#include "MILPValidatePass.h"

#include "BlockUtils.h"
#include "MILPContext.h"
#include "MILPOptions.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace {

constexpr const char *RegionStartMetadataKey = "milp.region.starts";

static cl::opt<unsigned> MILPValidateMaxStatesOpt(
    "milp-validate-max-states",
    cl::desc("Maximum exploration states per region during milp-validate"),
    cl::init(200000));

static cl::opt<unsigned> MILPValidateMaxPathLengthOpt(
    "milp-validate-max-path-length",
    cl::desc("Maximum path length during milp-validate (0 = auto)"),
    cl::init(0));

static cl::opt<bool> MILPValidateStrictOpt(
    "milp-validate-strict",
    cl::desc("Abort pass pipeline on validation failure"),
    cl::init(true));

struct EdgeKey {
    const BasicBlock *pred = nullptr;
    const BasicBlock *succ = nullptr;

    bool operator<(const EdgeKey &other) const {
        if (pred != other.pred) {
            return pred < other.pred;
        }
        return succ < other.succ;
    }
};

struct TraversalState {
    const BasicBlock *block = nullptr;
    double energyAtEntry_nJ = 0.0;
    SmallVector<unsigned, 8> loopBackedgeCounts;
    std::vector<std::string> path;
};

struct RegionValidationResult {
    std::string startBlock;
    bool passed = true;
    std::string failureReason;
    std::vector<std::string> failurePath;
    double failureEnergy_nJ = 0.0;
    double worstObservedEnergy_nJ = 0.0;
    std::vector<std::string> worstPath;
    unsigned exploredStates = 0;
};

struct MetadataReadResult {
    bool success = false;
    std::vector<std::string> regionStartNames;
    std::string errorMessage;
};

void collectLoopsRecursive(Loop *L, SmallVectorImpl<Loop*> &outLoops) {
    outLoops.push_back(L);
    for (Loop *sub : *L) {
        collectLoopsRecursive(sub, outLoops);
    }
}

MetadataReadResult readRegionStartMetadata(Function &F) {
    MetadataReadResult result;
    MDNode *node = F.getMetadata(RegionStartMetadataKey);
    if (!node) {
        result.errorMessage = "Missing function metadata '" +
                              std::string(RegionStartMetadataKey) + "'";
        return result;
    }
    if (node->getNumOperands() == 0) {
        result.errorMessage = "Region start metadata is empty";
        return result;
    }

    for (unsigned i = 0; i < node->getNumOperands(); ++i) {
        Metadata *operand = node->getOperand(i).get();
        auto *name = dyn_cast<MDString>(operand);
        if (!name) {
            result.errorMessage = "Region start metadata contains non-string operand";
            return result;
        }
        result.regionStartNames.push_back(name->getString().str());
    }

    result.success = true;
    return result;
}

void recordWorstPath(RegionValidationResult &result,
                     const std::vector<std::string> &path,
                     double energy_nJ) {
    if (energy_nJ >= result.worstObservedEnergy_nJ) {
        result.worstObservedEnergy_nJ = energy_nJ;
        result.worstPath = path;
    }
}

RegionValidationResult validateRegion(
    Function &F,
    const checkpoint::CFGAnalysis &cfg,
    const checkpoint::MILPParameters &params,
    const std::map<EdgeKey, const Loop*> &backedgeToLoop,
    const DenseMap<const Loop*, unsigned> &loopToIndex,
    const std::set<const BasicBlock*> &regionStarts,
    const BasicBlock *regionStart) {

    RegionValidationResult result;
    result.startBlock = checkpoint::getBlockName(*regionStart, F);

    const size_t loopCount = loopToIndex.size();
    const unsigned blockCount = cfg.getBlocks().size();
    const unsigned maxPathLength = (MILPValidateMaxPathLengthOpt == 0)
        ? std::max(32u, (blockCount + 1u) * std::max(1u, params.defaultLoopBound) * 4u)
        : MILPValidateMaxPathLengthOpt;

    TraversalState initial;
    initial.block = regionStart;
    initial.energyAtEntry_nJ = params.regionPrologueOverhead_nJ;
    initial.loopBackedgeCounts.assign(loopCount, 0);
    initial.path.push_back(result.startBlock);

    std::vector<TraversalState> stack;
    stack.push_back(std::move(initial));

    while (!stack.empty()) {
        TraversalState state = std::move(stack.back());
        stack.pop_back();

        if (++result.exploredStates > MILPValidateMaxStatesOpt) {
            result.passed = false;
            result.failureReason = "State exploration exceeded milp-validate-max-states";
            result.failurePath = state.path;
            return result;
        }

        if (state.path.size() > maxPathLength) {
            result.passed = false;
            result.failureReason = "Path length exceeded milp-validate-max-path-length";
            result.failurePath = state.path;
            return result;
        }

        const std::string blockName = checkpoint::getBlockName(*state.block, F);
        const double blockEnergy_nJ = cfg.getBlockInfo(blockName).energyCost;
        const double energyAfterBlock_nJ = state.energyAtEntry_nJ + blockEnergy_nJ;

        if (energyAfterBlock_nJ > params.energyBudget_nJ) {
            result.passed = false;
            result.failureReason = "Region budget exceeded inside block";
            result.failurePath = state.path;
            result.failureEnergy_nJ = energyAfterBlock_nJ;
            return result;
        }

        bool hasSucc = false;
        for (const BasicBlock *succ : successors(state.block)) {
            hasSucc = true;
            const std::string succName = checkpoint::getBlockName(*succ, F);

            if (regionStarts.count(succ)) {
                std::vector<std::string> terminatedPath = state.path;
                terminatedPath.push_back(succName);
                const double edgeEnergy_nJ =
                    energyAfterBlock_nJ + params.regionEpilogueOverhead_nJ;
                recordWorstPath(result, terminatedPath, edgeEnergy_nJ);
                if (edgeEnergy_nJ > params.energyBudget_nJ) {
                    result.passed = false;
                    result.failureReason = "Boundary transition exceeds energy budget";
                    result.failurePath = std::move(terminatedPath);
                    result.failureEnergy_nJ = edgeEnergy_nJ;
                    return result;
                }
                continue;
            }

            TraversalState next;
            next.block = succ;
            next.energyAtEntry_nJ = energyAfterBlock_nJ;
            next.loopBackedgeCounts = state.loopBackedgeCounts;
            next.path = state.path;
            next.path.push_back(succName);

            EdgeKey key{state.block, succ};
            auto backedgeIt = backedgeToLoop.find(key);
            if (backedgeIt != backedgeToLoop.end()) {
                const Loop *loop = backedgeIt->second;
                auto indexIt = loopToIndex.find(loop);
                if (indexIt != loopToIndex.end()) {
                    const unsigned idx = indexIt->second;
                    if (++next.loopBackedgeCounts[idx] > params.defaultLoopBound) {
                        continue;
                    }
                }
            }

            stack.push_back(std::move(next));
        }

        if (!hasSucc) {
            recordWorstPath(result, state.path, energyAfterBlock_nJ);
        }
    }

    return result;
}

} // namespace

namespace checkpoint {

PreservedAnalyses MILPValidatePass::run(Function &F, FunctionAnalysisManager &AM) {
    auto &LI = AM.getResult<LoopAnalysis>(F);
    auto contextResult = createMILPContext(
        F, LI, EnergyConfigOpt.getValue(), MILPConfigOpt.getValue(), "milp-validate pass");
    if (!contextResult.success()) {
        if (!contextResult.shouldSkip()) {
            errs() << contextResult.errorMessage;
            if (MILPValidateStrictOpt) {
                report_fatal_error("milp-validate setup failed");
            }
        }
        return PreservedAnalyses::all();
    }

    auto metadataResult = readRegionStartMetadata(F);
    if (!metadataResult.success) {
        errs() << "Error: milp-validate on function '" << F.getName()
               << "': " << metadataResult.errorMessage << "\n";
        if (MILPValidateStrictOpt) {
            report_fatal_error("milp-validate missing region metadata");
        }
        return PreservedAnalyses::all();
    }

    std::map<std::string, BasicBlock*> blockByName;
    for (BasicBlock &BB : F) {
        blockByName[getBlockName(BB, F)] = &BB;
    }

    std::set<const BasicBlock*> regionStarts;
    for (const std::string &name : metadataResult.regionStartNames) {
        auto it = blockByName.find(name);
        if (it == blockByName.end()) {
            errs() << "Error: milp-validate metadata references unknown block '" << name
                   << "' in function '" << F.getName() << "'\n";
            if (MILPValidateStrictOpt) {
                report_fatal_error("milp-validate metadata is invalid");
            }
            return PreservedAnalyses::all();
        }
        regionStarts.insert(it->second);
    }

    BasicBlock &entry = F.getEntryBlock();
    if (!regionStarts.count(&entry)) {
        errs() << "Error: milp-validate requires entry block to be a region start\n";
        if (MILPValidateStrictOpt) {
            report_fatal_error("milp-validate entry boundary missing");
        }
        return PreservedAnalyses::all();
    }

    SmallVector<Loop*, 16> loops;
    for (Loop *L : LI) {
        collectLoopsRecursive(L, loops);
    }
    DenseMap<const Loop*, unsigned> loopToIndex;
    for (unsigned i = 0; i < loops.size(); ++i) {
        loopToIndex[loops[i]] = i;
    }

    std::map<EdgeKey, const Loop*> backedgeToLoop;
    for (Loop *L : loops) {
        BasicBlock *header = L->getHeader();
        SmallVector<BasicBlock*, 8> latches;
        L->getLoopLatches(latches);
        for (BasicBlock *latch : latches) {
            backedgeToLoop[{latch, header}] = L;
        }
    }

    const MILPParameters &params = contextResult.context->milpParameters;
    errs() << "=== milp-validate on " << F.getName() << " ===\n";
    errs() << "  regions: " << regionStarts.size() << "\n";
    errs() << "  energy_budget_nJ: " << params.energyBudget_nJ << "\n";
    errs() << "  default_loop_bound: " << params.defaultLoopBound << "\n";

    bool overallPass = true;
    for (const BasicBlock *start : regionStarts) {
        RegionValidationResult result = validateRegion(
            F,
            *contextResult.context->cfg,
            params,
            backedgeToLoop,
            loopToIndex,
            regionStarts,
            start);

        errs() << "  Region start " << result.startBlock
               << ": explored_states=" << result.exploredStates
               << ", worst_energy_nJ=" << result.worstObservedEnergy_nJ << "\n";
        if (!result.passed) {
            overallPass = false;
            errs() << "    FAILURE: " << result.failureReason << "\n";
            if (result.failureEnergy_nJ > 0.0) {
                errs() << "    failure_energy_nJ: " << result.failureEnergy_nJ << "\n";
            }
            if (!result.failurePath.empty()) {
                errs() << "    witness_path: ";
                for (size_t i = 0; i < result.failurePath.size(); ++i) {
                    if (i > 0) {
                        errs() << " -> ";
                    }
                    errs() << result.failurePath[i];
                }
                errs() << "\n";
            }
        }
    }

    if (!overallPass) {
        errs() << "milp-validate FAILURE on function " << F.getName() << "\n";
        if (MILPValidateStrictOpt) {
            report_fatal_error("milp-validate failed");
        }
    } else {
        errs() << "milp-validate PASS on function " << F.getName() << "\n";
    }

    return PreservedAnalyses::all();
}

} // namespace checkpoint

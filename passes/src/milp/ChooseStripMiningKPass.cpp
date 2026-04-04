#include "milp/ChooseStripMiningKPass.h"

#include "common/CFGAnalysis.h"
#include "common/Logger.h"
#include "common/LoopTripCount.h"
#include "estimator/EnergyEstimatorFactory.h"
#include "milp/EnergyModel.h"
#include "milp/StateAnalysis.h"
#include "milp/StripMiningMetadata.h"
#include "milp/StripMiningSummary.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/CommandLine.h"

#include <optional>
#include <string>
#include <vector>

using namespace llvm;

extern cl::opt<std::string> EnergyConfigOpt;
extern cl::opt<std::string> MILPConfigOpt;

namespace {

using checkpoint::getMarkerTripCount;
using checkpoint::getStripMiningKindMetadata;
using checkpoint::getStripMiningKRoleMetadata;
using checkpoint::getStripMiningOriginalTripCountMetadata;
using checkpoint::getStripMiningRoleMetadata;
using checkpoint::hasNoSummaryLoopMetadata;
using checkpoint::hasStripMinedLoopMetadata;
using checkpoint::removeLoopTripCountMetadata;
using checkpoint::removeStripMinedLoopMetadata;
using checkpoint::setLoopTripCountMetadata;
using checkpoint::setNoSummaryLoopMetadata;

static std::vector<Loop *> collectLoops(LoopInfo &LI) {
    std::vector<Loop *> loops;
    SmallVector<Loop *, 16> worklist(LI.begin(), LI.end());
    while (!worklist.empty()) {
        Loop *L = worklist.pop_back_val();
        loops.push_back(L);
        for (Loop *Sub : *L)
            worklist.push_back(Sub);
    }
    return loops;
}

static void refreshBlockEnergy(Function &F, checkpoint::EnergyEstimator &estimator,
                               DenseMap<const BasicBlock *, double> &blockEnergy) {
    estimator.prepareForFunction(F);
    blockEnergy.clear();
    blockEnergy.reserve(F.size());
    for (BasicBlock &BB : F)
        blockEnergy[&BB] = estimator.estimate(BB).cost;
}

static bool updateConstantOperand(Instruction *I, uint64_t newValue) {
    for (unsigned i = 0; i < I->getNumOperands(); i++) {
        auto *CI = dyn_cast<ConstantInt>(I->getOperand(i));
        if (!CI)
            continue;
        I->setOperand(i, ConstantInt::get(CI->getType(), newValue));
        return true;
    }
    return false;
}

static bool patchChunkedLoop(Loop *TargetLoop, uint64_t newK, uint64_t originalTripCountUpper) {
    Instruction *CounterCmp = nullptr;
    for (BasicBlock *BB : TargetLoop->blocks()) {
        for (Instruction &I : *BB) {
            auto role = getStripMiningKRoleMetadata(&I);
            if (role && *role == "chunked.counter-bound") {
                CounterCmp = &I;
                break;
            }
        }
        if (CounterCmp)
            break;
    }
    if (!CounterCmp || !updateConstantOperand(CounterCmp, newK))
        return false;

    setLoopTripCountMetadata(TargetLoop, newK);
    if (Loop *OuterLoop = TargetLoop->getParentLoop()) {
        if (originalTripCountUpper == 0) {
            removeLoopTripCountMetadata(OuterLoop);
        } else {
            uint64_t outerTripCountUpper = 1 + ((originalTripCountUpper - 1) / newK);
            setLoopTripCountMetadata(OuterLoop, outerTripCountUpper);
        }
    }
    return true;
}

static void disableSummaryWithWarning(Function &F, Loop *L,
                                      const checkpoint::StripMiningBudgetResult &budget,
                                      uint64_t originalTripCount) {
    removeStripMinedLoopMetadata(L);
    setNoSummaryLoopMetadata(L);
    std::string headerName = L && L->getHeader() ? L->getHeader()->getName().str() : "<unknown>";
    std::string details = budget.details;
    if (originalTripCount > 0) {
        if (!details.empty())
            details += ", ";
        details += "orig-trip-count=" + std::to_string(originalTripCount);
    }
    PLOGW << "ChooseStripMiningKPass warning: disabling strip-mined summary " << F.getName()
          << "::" << headerName << " reason=no-useful-k " << details;
}

} // namespace

namespace checkpoint {

PreservedAnalyses ChooseStripMiningKPass::run(Function &F, FunctionAnalysisManager &AM) {
    checkpoint::initLogging();

    if (F.isDeclaration())
        return PreservedAnalyses::all();
    if (MILPConfigOpt.getValue().empty() || EnergyConfigOpt.getValue().empty())
        return PreservedAnalyses::all();

    auto milpParamsOpt = parseMILPEnergyParams(MILPConfigOpt.getValue());
    if (!milpParamsOpt)
        return PreservedAnalyses::all();

    auto factory = EnergyEstimatorFactory::createDefault();
    std::unique_ptr<EnergyEstimator> estimator =
        factory.createFromConfig(EnergyConfigOpt.getValue());
    if (!estimator)
        return PreservedAnalyses::all();

    auto &LI = AM.getResult<LoopAnalysis>(F);
    auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    auto &AA = AM.getResult<AAManager>(F);
    checkpoint::CFGAnalysis cfg(F, LI, *estimator);
    checkpoint::StateAnalysis state(F, AA, cfg);
    if (state.hasAnalysisErrors()) {
        state.printAnalysisErrors(errs());
        return PreservedAnalyses::all();
    }

    DenseMap<const BasicBlock *, double> blockEnergy;
    refreshBlockEnergy(F, *estimator, blockEnergy);

    std::vector<Loop *> allLoops = collectLoops(LI);
    bool changed = false;

    for (Loop *L : allLoops) {
        if (!hasStripMinedLoopMetadata(L) || hasNoSummaryLoopMetadata(L))
            continue;

        auto role = getStripMiningRoleMetadata(L);
        if (!role || *role != "target")
            continue;

        auto kind = getStripMiningKindMetadata(L);
        auto originalTripCount = getStripMiningOriginalTripCountMetadata(L);
        auto currentK = getMarkerTripCount(L);
        if (!kind || !originalTripCount || !currentK) {
            PLOGW << "ChooseStripMiningKPass warning: missing strip-mining metadata in "
                  << F.getName() << "::" << L->getHeader()->getName();
            continue;
        }
        if (*kind != "chunked")
            continue;

        StripMiningBudgetResult budget =
            computeStripMiningBudgetResult(L, blockEnergy, LI, SE, state, *milpParamsOpt);
        uint64_t maxK = budget.ok ? budget.maxK : 0;
        uint64_t finalK = maxK;
        if (*originalTripCount > 0 && finalK > *originalTripCount)
            finalK = *originalTripCount;

        if (!budget.ok || finalK <= 1) {
            disableSummaryWithWarning(F, L, budget, *originalTripCount);
            changed = true;
            continue;
        }

        if (finalK == *currentK)
            continue;

        bool updated = patchChunkedLoop(L, finalK, *originalTripCount);

        if (!updated) {
            PLOGW << "ChooseStripMiningKPass warning: failed to retune " << F.getName()
                  << "::" << L->getHeader()->getName() << " kind=" << *kind
                  << " current-k=" << *currentK << " final-k=" << finalK;
            continue;
        }

        SE.forgetLoop(L);
        changed = true;
        PLOGD << "ChooseStripMiningKPass: updated " << F.getName()
              << "::" << L->getHeader()->getName() << " kind=" << *kind
              << " current-k=" << *currentK << " final-k=" << finalK << " max-k=" << maxK;
    }

    if (verifyFunction(F, &errs())) {
        PLOGE << "ChooseStripMiningKPass: verifier reported errors in " << F.getName();
    }

    return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace checkpoint

#include "CheckpointPass.h"
#include "CheckpointAnalysisPass.h"
#include "CFGAnalysis.h"
#include "CheckpointOptimizer.h"
#include "EnergyConfig.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/CommandLine.h"


using namespace llvm;

// Command line options (visible to other translation units)
cl::opt<std::string> EnergyConfigOpt(
    "energy-config",
    cl::desc("Path to JSON energy configuration file (required)"),
    cl::value_desc("filename"),
    cl::Required);

namespace {

static cl::opt<std::string> CheckpointFnOpt(
    "checkpoint-function",
    cl::desc("Name of checkpoint function to insert"),
    cl::init("__checkpoint"));

} // anonymous namespace

namespace checkpoint {

PreservedAnalyses CheckpointPass::run(Function &F,
                                       FunctionAnalysisManager &AM) {
    // Load configuration on first invocation
    if (!EnergyConfig::isLoaded()) {
        EnergyConfig::loadFromFile(EnergyConfigOpt);
    }

    // Skip declarations
    if (F.isDeclaration()) {
        return PreservedAnalyses::all();
    }

    double capacity = EnergyConfig::getCapacity();
    std::string checkpointFnName = CheckpointFnOpt;


    // Step 1: Get loop info from LLVM
    auto &LI = AM.getResult<LoopAnalysis>(F);

    // Step 2: Analyze CFG
    CFGAnalysis cfg(F, LI);

    // Step 3: Check feasibility
    CheckpointOptimizer optimizer(cfg, capacity);
    auto infeasible = optimizer.getInfeasibleBlocks();
    if (!infeasible.empty()) {
        errs() << "Error: The following blocks exceed energy capacity:\n";
        for (const auto &block : infeasible) {
            errs() << "  " << block << " (cost: "
                   << cfg.getBlockInfo(block).energyCost
                   << ", capacity: " << capacity << ")\n";
        }
        return PreservedAnalyses::all();
    }

    // Step 4: Solve MILP
    if (!optimizer.solve()) {
        errs() << "Optimization failed\n";
        return PreservedAnalyses::all();
    }

    auto checkpoints = optimizer.getCheckpoints();

    if (checkpoints.empty()) {
        errs() << "No checkpoints needed for function " << F.getName() << "\n";
        return PreservedAnalyses::all();
    }

    errs() << "Inserting " << checkpoints.size() << " checkpoint(s) in "
           << F.getName() << ":\n";
    for (const auto &cp : checkpoints) {
        errs() << "  " << cp << "\n";
    }

    // Step 5: Instrument - insert checkpoint calls
    Module *M = F.getParent();
    LLVMContext &Ctx = M->getContext();

    // Get or declare: void __checkpoint(const char*)
    FunctionCallee CheckpointCallee = M->getOrInsertFunction(
        checkpointFnName,
        Type::getVoidTy(Ctx),
        PointerType::get(Ctx, 0)  // ptr (opaque pointer)
    );

    // Insert checkpoint calls
    for (BasicBlock &BB : F) {
        std::string blockName = BB.getName().str();
        if (blockName.empty()) {
            // Handle unnamed blocks
            size_t idx = 0;
            for (BasicBlock &B : F) {
                if (&B == &BB) {
                    blockName = "bb" + std::to_string(idx);
                    break;
                }
                idx++;
            }
        }

        if (checkpoints.count(blockName)) {
            // Insert after PHI nodes using iterator
            BasicBlock::iterator InsertPt = BB.getFirstNonPHIIt();
            IRBuilder<> Builder(&*InsertPt);

            // Create global string using new API (CreateGlobalString returns GlobalVariable*)
            // We need to cast it to ptr using a constant GEP expression
            GlobalVariable *StrGV = Builder.CreateGlobalString(blockName, "checkpoint_name");

            // Get pointer to first element using constant expression GEP
            Constant *Zero = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
            Constant *Indices[] = {Zero, Zero};
            Constant *StrPtr = ConstantExpr::getInBoundsGetElementPtr(
                StrGV->getValueType(), StrGV, Indices);

            Builder.CreateCall(CheckpointCallee, {StrPtr});
        }
    }

    // We modified the IR
    return PreservedAnalyses::none();
}

} // namespace checkpoint

// Plugin registration for new pass manager
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
    return {
        LLVM_PLUGIN_API_VERSION,
        "CheckpointPasses",
        LLVM_VERSION_STRING,
        [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                    if (Name == "checkpoint") {
                        FPM.addPass(checkpoint::CheckpointPass());
                        return true;
                    }
                    if (Name == "checkpoint-analysis") {
                        FPM.addPass(checkpoint::CheckpointAnalysisPass());
                        return true;
                    }
                    return false;
                });
        }
    };
}

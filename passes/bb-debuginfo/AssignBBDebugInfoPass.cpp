#include "AssignBBDebugInfoPass.h"

#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

namespace {

static cl::opt<bool> QuietMode(
    "assign-bb-quiet",
    cl::desc("Suppress informational messages from assign-bb-debuginfo pass"),
    cl::init(false));

} // anonymous namespace

namespace bbdebuginfo {

PreservedAnalyses AssignBBDebugInfoPass::run(Function &F,
                                              FunctionAnalysisManager &AM) {
    // Skip functions without debug info
    DISubprogram *SP = F.getSubprogram();
    if (!SP) {
        if (!QuietMode) {
            errs() << "Warning: Skipping function '" << F.getName()
                   << "' - no debug info (compile with -g)\n";
        }
        return PreservedAnalyses::all();
    }

    LLVMContext &Ctx = F.getContext();
    unsigned BBIndex = 0;
    unsigned totalLabeled = 0;

    for (BasicBlock &BB : F) {
        // Create unique debug location: line = BBIndex, column = 0
        DILocation *Loc = DILocation::get(Ctx,
                                           /*Line=*/BBIndex,
                                           /*Column=*/0,
                                           /*Scope=*/SP);
        DebugLoc DL(Loc);

        unsigned labeledInBB = 0;
        bool hasNonPHI = false;

        for (Instruction &I : BB) {
            // Skip PHI nodes - they don't generate direct assembly instructions
            // Code for PHI resolution appears in predecessor blocks
            if (!isa<PHINode>(&I)) {
                hasNonPHI = true;
                I.setDebugLoc(DL);
                labeledInBB++;
            }
        }

        // Diagnostic for corner cases
        if (!QuietMode) {
            if (BB.empty()) {
                errs() << "Note: BB " << BBIndex << " in '" << F.getName()
                       << "' is empty (no instructions to label)\n";
            } else if (!hasNonPHI) {
                errs() << "Note: BB " << BBIndex << " in '" << F.getName()
                       << "' has only PHI nodes (no labelable instructions)\n";
            }
        }

        totalLabeled += labeledInBB;
        BBIndex++;
    }

    if (!QuietMode) {
        errs() << "Labeled " << totalLabeled << " instructions across "
               << BBIndex << " basic blocks in '" << F.getName() << "'\n";
    }

    // We modified IR metadata
    return PreservedAnalyses::none();
}

} // namespace bbdebuginfo

// Plugin registration for new pass manager
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
    return {
        LLVM_PLUGIN_API_VERSION,
        "BBDebugInfoPass",
        LLVM_VERSION_STRING,
        [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                    if (Name == "assign-bb-debuginfo") {
                        FPM.addPass(bbdebuginfo::AssignBBDebugInfoPass());
                        return true;
                    }
                    return false;
                });
        }
    };
}

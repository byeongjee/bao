#include "milp/CheckpointPass.h"
#include "milp/CheckpointAnalysisPass.h"
#include "rockclimb/RockClimbPass.h"

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

// Shared command line option for energy config path.
// Referenced via extern from CheckpointPass.cpp and CheckpointAnalysisPass.cpp.
cl::opt<std::string> EnergyConfigOpt(
    "energy-config",
    cl::desc("Path to JSON energy configuration file (required for checkpoint passes)"),
    cl::value_desc("filename"),
    cl::init(""));

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
                    if (Name == "milp") {
                        FPM.addPass(checkpoint::CheckpointPass());
                        return true;
                    }
                    if (Name == "checkpoint-analysis") {
                        FPM.addPass(checkpoint::CheckpointAnalysisPass());
                        return true;
                    }
                    if (Name == "rockclimb") {
                        FPM.addPass(checkpoint::RockClimbPass());
                        return true;
                    }
                    return false;
                });
        }
    };
}

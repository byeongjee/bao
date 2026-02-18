#include "common/EnergyValidatorPass.h"
#include "milp/MILPCheckpointPass.h"
#include "milp/LoopChunkingPass.h"
#include "rockclimb/RockClimbPass.h"

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/LCSSA.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"

using namespace llvm;

// Shared command line option for energy config path.
// Referenced via extern from MILPCheckpointPass.cpp.
cl::opt<std::string> EnergyConfigOpt(
    "energy-config",
    cl::desc("Path to JSON energy configuration file (required for checkpoint passes)"),
    cl::value_desc("filename"),
    cl::init(""));

cl::opt<std::string> MILPConfigOpt(
    "milp-config",
    cl::desc("Path to JSON MILP configuration file (required for MILP passes)"),
    cl::value_desc("filename"),
    cl::init(""));

cl::opt<bool> AcceptFeasibleOpt(
    "milp-accept-feasible",
    cl::desc("Accept feasible (non-optimal) MILP solutions (e.g., from time limit)"),
    cl::init(false));

cl::opt<bool> LoopChunkingEnabledOpt(
    "loop-chunking-enabled",
    cl::desc("Enable loop chunking before MILP passes"),
    cl::init(false));

cl::opt<bool> AddDebugMarkersOpt(
    "add-debug-markers",
    cl::desc("Emit runtime function calls for register save/restore (for mock counter debugging)"),
    cl::init(false));

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
                        FPM.addPass(LoopSimplifyPass());
                        FPM.addPass(LCSSAPass());
                        FPM.addPass(checkpoint::LoopChunkingPass());
                        FPM.addPass(checkpoint::MILPCheckpointPass());
                        return true;
                    }
                    if (Name == "milp") {
                        FPM.addPass(LoopSimplifyPass());
                        FPM.addPass(LCSSAPass());
                        FPM.addPass(checkpoint::LoopChunkingPass());
                        FPM.addPass(checkpoint::MILPCheckpointPass());
                        return true;
                    }
                    if (Name == "rockclimb") {
                        FPM.addPass(checkpoint::RockClimbPass());
                        return true;
                    }
                    if (Name == "energy-validate") {
                        FPM.addPass(checkpoint::EnergyValidatorPass());
                        return true;
                    }
                    return false;
                });
        }
    };
}

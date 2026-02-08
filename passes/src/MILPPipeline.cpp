#include "CheckpointInsertPass.h"
#include "MILPNextPass.h"
#include "MILPOptions.h"
#include "MILPValidatePass.h"
#include "RockClimbPass.h"

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

namespace checkpoint {

cl::opt<std::string> EnergyConfigOpt(
    "energy-config",
    cl::desc("Path to JSON energy estimator configuration file"),
    cl::value_desc("filename"),
    cl::init(""));

cl::opt<std::string> MILPConfigOpt(
    "milp-config",
    cl::desc("Path to JSON MILP parameter file"),
    cl::value_desc("filename"),
    cl::init(""));

cl::opt<std::string> CheckpointAlgorithmOpt(
    "checkpoint-algorithm",
    cl::desc("Algorithm for checkpoint-insert pass"),
    cl::value_desc("milp|rockclimb"),
    cl::init("milp"));

} // namespace checkpoint

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
    return {
        LLVM_PLUGIN_API_VERSION,
        "CheckpointInsertionPasses",
        LLVM_VERSION_STRING,
        [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef name,
                   FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                    if (name == "milp") {
                        FPM.addPass(checkpoint::MILPNextPass());
                        return true;
                    }
                    if (name == "rockclimb") {
                        FPM.addPass(checkpoint::RockClimbPass());
                        return true;
                    }
                    if (name == "checkpoint-insert") {
                        FPM.addPass(checkpoint::CheckpointInsertPass());
                        return true;
                    }
                    if (name == "milp-next") {
                        FPM.addPass(checkpoint::MILPNextPass());
                        return true;
                    }
                    if (name == "milp-validate") {
                        FPM.addPass(checkpoint::MILPValidatePass());
                        return true;
                    }
                    return false;
                });
        }
    };
}

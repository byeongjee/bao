#include "common/EnergyValidatorPass.h"
#include "common/TripCountAnnotationPass.h"
#include "milp/LoopStripMiningPass.h"
#include "milp/MILPCheckpointPass.h"
#include "rockclimb/RockClimbPass.h"
#include "schematic/SchematicPass.h"

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Scalar/IndVarSimplify.h"
#include "llvm/Transforms/Scalar/LoopRotation.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include "llvm/Transforms/Utils/LCSSA.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"

using namespace llvm;

// Shared command line option for energy config path.
// Referenced via extern from MILPCheckpointPass.cpp.
cl::opt<std::string>
    EnergyConfigOpt("energy-config",
                    cl::desc("Path to JSON energy configuration file (required "
                             "for checkpoint passes)"),
                    cl::value_desc("filename"), cl::init(""));

cl::opt<std::string> MILPConfigOpt(
    "milp-config",
    cl::desc("Path to JSON MILP configuration file (required for MILP passes)"),
    cl::value_desc("filename"), cl::init(""));

cl::opt<bool> AcceptFeasibleOpt(
    "milp-accept-feasible",
    cl::desc(
        "Accept feasible (non-optimal) MILP solutions (e.g., from time limit)"),
    cl::init(false));

cl::opt<double> MILPTimeLimitOpt(
    "milp-time-limit",
    cl::desc("Time limit for MILP solver in seconds (default: 600)"),
    cl::value_desc("seconds"), cl::init(600.0));

cl::opt<bool> LoopStripMiningEnabledOpt(
    "loop-strip-mining-enabled",
    cl::desc("Enable loop strip-mining before MILP passes"), cl::init(false));

cl::opt<bool>
    AddDebugMarkersOpt("add-debug-markers",
                       cl::desc("Emit runtime function calls for register "
                                "save/restore (for mock counter debugging)"),
                       cl::init(false));

cl::opt<std::string> SchematicConfigOpt(
    "schematic-config",
    cl::desc("Path to JSON SCHEMATIC configuration file"),
    cl::value_desc("filename"), cl::init(""));

// Plugin registration for new pass manager
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "CheckpointPasses", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "checkpoint") {
                    FPM.addPass(LoopSimplifyPass());
                    FPM.addPass(LCSSAPass());
                    FPM.addPass(
                        createFunctionToLoopPassAdaptor(LoopRotatePass()));
                    FPM.addPass(
                        createFunctionToLoopPassAdaptor(IndVarSimplifyPass()));
                    FPM.addPass(checkpoint::LoopStripMiningPass());
                    FPM.addPass(checkpoint::MILPCheckpointPass());
                    return true;
                  }
                  if (Name == "tripcount-annotation") {
                    FPM.addPass(checkpoint::TripCountAnnotationPass());
                    return true;
                  }
                  if (Name == "milp") {
                    FPM.addPass(LoopSimplifyPass());
                    FPM.addPass(LCSSAPass());
                    FPM.addPass(
                        createFunctionToLoopPassAdaptor(LoopRotatePass()));
                    FPM.addPass(
                        createFunctionToLoopPassAdaptor(IndVarSimplifyPass()));
                    FPM.addPass(checkpoint::LoopStripMiningPass());
                    FPM.addPass(checkpoint::MILPCheckpointPass());
                    return true;
                  }
                  if (Name == "rockclimb") {
                    FPM.addPass(LoopSimplifyPass());
                    FPM.addPass(LCSSAPass());
                    FPM.addPass(checkpoint::RockClimbPass());
                    return true;
                  }
                  if (Name == "schematic") {
                    FPM.addPass(LoopSimplifyPass());
                    FPM.addPass(LCSSAPass());
                    FPM.addPass(checkpoint::SchematicPass());
                    FPM.addPass(PromotePass()); // mem2reg: promote loop counter allocas to SSA
                    return true;
                  }
                  if (Name == "energy-validate") {
                    FPM.addPass(checkpoint::EnergyValidatorPass());
                    return true;
                  }
                  return false;
                });
          }};
}

#include "common/BBFreqCollectorPass.h"
#include "common/TripCountAnnotationPass.h"
#include "milp/AllocaToGlobalPass.h"
#include "milp/LoopStripMiningPass.h"
#include "milp/MILPCheckpointPass.h"
#include "rockclimb/RockClimbLoopUnrollPass.h"
#include "schematic/CallIsolation.h"
#include "schematic/SchematicPass.h"
#include "schematic/TraceCollectorPass.h"

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Scalar/IndVarSimplify.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include "llvm/Transforms/Scalar/LoopRotation.h"
#include "llvm/Transforms/Utils/LCSSA.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"

using namespace llvm;

// Shared command line option for energy config path.
// Referenced via extern from MILPCheckpointPass.cpp.
cl::opt<std::string> EnergyConfigOpt("energy-config",
                                     cl::desc("Path to JSON energy configuration file (required "
                                              "for checkpoint passes)"),
                                     cl::value_desc("filename"), cl::init(""));

cl::opt<std::string>
    MILPConfigOpt("milp-config",
                  cl::desc("Path to JSON MILP configuration file (required for MILP passes)"),
                  cl::value_desc("filename"), cl::init(""));

cl::opt<bool> AcceptFeasibleOpt(
    "milp-accept-feasible",
    cl::desc("Accept feasible (non-optimal) MILP solutions (e.g., from time limit)"),
    cl::init(false));

cl::opt<double> MILPTimeLimitOpt("milp-time-limit",
                                 cl::desc("Time limit for MILP solver in seconds (default: 600)"),
                                 cl::value_desc("seconds"), cl::init(600.0));

cl::opt<double> MILPGapOpt("milp-gap",
                           cl::desc("MIP optimality gap tolerance (default: 0.0 = proven optimal)"),
                           cl::value_desc("fraction"), cl::init(0.0));

cl::opt<std::string> MILPLogFileOpt("milp-log-file",
                                    cl::desc("Gurobi log file path (empty = no logging)"),
                                    cl::value_desc("path"), cl::init(""));

cl::opt<bool> MILPCoarseAllocationOpt(
    "milp-coarse-allocation",
    cl::desc("Use one VM placement variable per eligible value instead of per-region placement"),
    cl::init(false));

cl::opt<bool> LoopStripMiningEnabledOpt("loop-strip-mining-enabled",
                                        cl::desc("Enable loop strip-mining before MILP passes"),
                                        cl::init(false));

cl::opt<bool> AddDebugMarkersOpt("add-debug-markers",
                                 cl::desc("Emit runtime function calls for register "
                                          "save/restore (for mock counter debugging)"),
                                 cl::init(false));

cl::opt<std::string> SchematicConfigOpt("schematic-config",
                                        cl::desc("Path to JSON SCHEMATIC configuration file"),
                                        cl::value_desc("filename"), cl::init(""));

cl::opt<std::string> SchematicTraceOpt("schematic-trace",
                                       cl::desc("Path to trace JSON for SCHEMATIC"),
                                       cl::value_desc("filename"), cl::init(""));

cl::opt<std::string>
    BBFreqFileOpt("bb-freq-file",
                  cl::desc("Path to BB frequency JSON file (from bb-freq-collect runtime)"),
                  cl::value_desc("filename"), cl::init(""));

// Plugin registration for new pass manager
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return {LLVM_PLUGIN_API_VERSION, "CheckpointPasses", LLVM_VERSION_STRING, [](PassBuilder &PB) {
                PB.registerPipelineParsingCallback([](StringRef Name, FunctionPassManager &FPM,
                                                      ArrayRef<PassBuilder::PipelineElement>) {
                    if (Name == "checkpoint") {
                        FPM.addPass(checkpoint::AllocaToGlobalPass());
                        FPM.addPass(LoopSimplifyPass());
                        FPM.addPass(LCSSAPass());
                        FPM.addPass(createFunctionToLoopPassAdaptor(LoopRotatePass()));
                        FPM.addPass(createFunctionToLoopPassAdaptor(IndVarSimplifyPass()));
                        FPM.addPass(checkpoint::LoopStripMiningPass());
                        FPM.addPass(checkpoint::MILPCheckpointPass());
                        return true;
                    }
                    if (Name == "tripcount-annotation") {
                        FPM.addPass(checkpoint::TripCountAnnotationPass());
                        return true;
                    }
                    if (Name == "milp") {
                        FPM.addPass(checkpoint::AllocaToGlobalPass());
                        FPM.addPass(LoopSimplifyPass());
                        FPM.addPass(LCSSAPass());
                        FPM.addPass(createFunctionToLoopPassAdaptor(LoopRotatePass()));
                        FPM.addPass(createFunctionToLoopPassAdaptor(IndVarSimplifyPass()));
                        FPM.addPass(checkpoint::LoopStripMiningPass());
                        FPM.addPass(checkpoint::MILPCheckpointPass());
                        return true;
                    }
                    if (Name == "trace-collect") {
                        FPM.addPass(LoopSimplifyPass());
                        FPM.addPass(LCSSAPass());
                        FPM.addPass(checkpoint::TraceCollectorPass());
                        return true;
                    }
                    if (Name == "bb-freq-collect") {
                        FPM.addPass(checkpoint::AllocaToGlobalPass());
                        FPM.addPass(LoopSimplifyPass());
                        FPM.addPass(LCSSAPass());
                        FPM.addPass(createFunctionToLoopPassAdaptor(LoopRotatePass()));
                        FPM.addPass(createFunctionToLoopPassAdaptor(IndVarSimplifyPass()));
                        FPM.addPass(checkpoint::LoopStripMiningPass());
                        FPM.addPass(checkpoint::BBFreqCollectorPass());
                        return true;
                    }
                    if (Name == "milp-preprocess") {
                        FPM.addPass(checkpoint::AllocaToGlobalPass());
                        FPM.addPass(LoopSimplifyPass());
                        FPM.addPass(LCSSAPass());
                        FPM.addPass(createFunctionToLoopPassAdaptor(LoopRotatePass()));
                        FPM.addPass(createFunctionToLoopPassAdaptor(IndVarSimplifyPass()));
                        FPM.addPass(checkpoint::LoopStripMiningPass());
                        return true;
                    }
                    if (Name == "rockclimb-preprocess") {
                        FPM.addPass(LoopSimplifyPass());
                        FPM.addPass(LCSSAPass());
                        FPM.addPass(createFunctionToLoopPassAdaptor(LoopRotatePass()));
                        FPM.addPass(createFunctionToLoopPassAdaptor(IndVarSimplifyPass()));
                        FPM.addPass(checkpoint::RockClimbLoopUnrollPass());
                        return true;
                    }
                    if (Name == "milp-reclamp-only") {
                        FPM.addPass(checkpoint::LoopStripMiningPass(/*reclampOnly=*/true));
                        return true;
                    }
                    if (Name == "milp-solve-only") {
                        FPM.addPass(checkpoint::MILPCheckpointPass());
                        return true;
                    }
                    if (Name == "bb-freq-collect-only") {
                        FPM.addPass(checkpoint::BBFreqCollectorPass());
                        return true;
                    }
                    return false;
                });
                // Module-level passes (inter-procedural). Registered separately
                // because the new PM matches pipeline names per IR-unit type.
                PB.registerPipelineParsingCallback([](StringRef Name, ModulePassManager &MPM,
                                                      ArrayRef<PassBuilder::PipelineElement>) {
                    if (Name == "schematic-isolate") {
                        MPM.addPass(checkpoint::CallIsolationPass());
                        return true;
                    }
                    if (Name == "schematic") {
                        // Canonicalize loops per function, then run the
                        // inter-procedural SCHEMATIC module pass.
                        FunctionPassManager FPM;
                        FPM.addPass(LoopSimplifyPass());
                        FPM.addPass(LCSSAPass());
                        MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));
                        MPM.addPass(checkpoint::SchematicPass());
                        return true;
                    }
                    return false;
                });
            }};
}

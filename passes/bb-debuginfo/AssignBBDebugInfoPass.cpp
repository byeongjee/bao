#include "AssignBBDebugInfoPass.h"

#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include <nlohmann/json.hpp>
#include <fstream>

using namespace llvm;

namespace {

static cl::opt<bool> QuietMode(
    "assign-bb-quiet",
    cl::desc("Suppress informational messages from assign-bb-debuginfo pass"),
    cl::init(false));

static cl::opt<std::string> MappingOutput(
    "bb-mapping",
    cl::desc("Output path for BB index-to-name mapping JSON"),
    cl::init(""));

} // anonymous namespace

namespace bbdebuginfo {

PreservedAnalyses AssignBBDebugInfoPass::run(Module &M,
                                              ModuleAnalysisManager &AM) {
    LLVMContext &Ctx = M.getContext();

    // Check if there are any functions with bodies to process
    bool hasFunctions = false;
    for (Function &F : M) {
        if (!F.isDeclaration()) {
            hasFunctions = true;
            break;
        }
    }

    if (!hasFunctions) {
        if (!QuietMode) {
            errs() << "Warning: Module has no function definitions to label\n";
        }
        return PreservedAnalyses::all();
    }

    // Remove any existing debug info to start fresh
    // This includes named metadata like llvm.dbg.cu
    if (NamedMDNode *DbgCU = M.getNamedMetadata("llvm.dbg.cu")) {
        M.eraseNamedMetadata(DbgCU);
    }

    // Strip existing subprograms from functions
    for (Function &F : M) {
        if (!F.isDeclaration()) {
            F.setSubprogram(nullptr);
        }
    }

    // Create fresh debug info using DIBuilder
    DIBuilder DIB(M);

    // Create a dummy file for our debug info
    DIFile *File = DIB.createFile("bb-labels.ll", "");

    // Create a compile unit
    // Using DW_LANG_C since it's a simple, well-supported language
    DICompileUnit *CU = DIB.createCompileUnit(
        dwarf::DW_LANG_C,
        File,
        "bb-debuginfo-pass",  // Producer
        false,                // isOptimized
        "",                   // Flags
        0                     // Runtime version
    );

    // Create a simple void function type for all functions
    // We don't need accurate types for BB labeling
    DISubroutineType *FnTy = DIB.createSubroutineType(
        DIB.getOrCreateTypeArray({})  // No parameters, no return type info
    );

    // Process each function
    unsigned totalFunctions = 0;
    unsigned totalBBs = 0;
    unsigned totalInstructions = 0;

    // Mapping from function name -> BB index -> BB name
    nlohmann::json bbMapping;

    for (Function &F : M) {
        // Skip declarations (external functions)
        if (F.isDeclaration()) {
            continue;
        }

        // Create a fresh DISubprogram for this function
        // Line 0 and ScopeLine 0 mean "unmapped" - no source location
        // This prevents LLC from generating stray source line numbers
        DISubprogram *SP = DIB.createFunction(
            File,                    // Scope (use file as scope)
            F.getName(),             // Name
            F.getName(),             // Linkage name (same as name)
            File,                    // File
            0,                       // Line number (0 = unmapped)
            FnTy,                    // Type
            0,                       // Scope line (0 = unmapped)
            DINode::FlagZero,        // Flags
            DISubprogram::SPFlagDefinition  // SP flags
        );

        F.setSubprogram(SP);

        // BB indices start from 1 (line 0 is reserved for unmapped code)
        unsigned BBIndex = 1;
        unsigned BBCount = 0;
        unsigned funcLabeled = 0;

        // Collect BB mapping for this function
        nlohmann::json funcMapping;

        for (BasicBlock &BB : F) {
            // Create unique debug location: line = BBIndex, column = 0
            DILocation *Loc = DILocation::get(Ctx,
                                               /*Line=*/BBIndex,
                                               /*Column=*/0,
                                               /*Scope=*/SP);
            DebugLoc DL(Loc);

            // Store BB name mapping
            std::string bbName;
            if (BB.hasName()) {
                bbName = BB.getName().str();
            } else {
                // Use 0-based index for unnamed BBs to match BlockUtils.h pattern
                bbName = "bb" + std::to_string(BBIndex - 1);
            }
            funcMapping[std::to_string(BBIndex)] = bbName;

            unsigned labeledInBB = 0;
            bool hasNonPHI = false;

            for (Instruction &I : BB) {
                // Skip PHI nodes - they don't generate direct assembly instructions
                // Code for PHI resolution appears in predecessor blocks
                if (!isa<PHINode>(&I)) {
                    hasNonPHI = true;
                    I.setDebugLoc(DL);
                    // Remove loop metadata - it may contain references to old debug info
                    if (I.getMetadata(LLVMContext::MD_loop)) {
                        I.setMetadata(LLVMContext::MD_loop, nullptr);
                    }
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

            funcLabeled += labeledInBB;
            BBIndex++;
            BBCount++;
        }

        if (!QuietMode) {
            errs() << "Labeled " << funcLabeled << " instructions across "
                   << BBCount << " basic blocks in '" << F.getName() << "'\n";
        }

        // Store function mapping
        bbMapping[F.getName().str()] = funcMapping;

        totalFunctions++;
        totalBBs += BBCount;
        totalInstructions += funcLabeled;
    }

    // Finalize the debug info
    DIB.finalize();

    // Set module flags for DWARF - required for LLC to emit valid debug info
    // Use DWARF v4 for compatibility with older toolchains (MSP430 GCC objdump
    // doesn't support DWARF v5 string/address index forms like DW_FORM_strx1)
    M.addModuleFlag(Module::Max, "Dwarf Version", 4);
    M.addModuleFlag(Module::Max, "Debug Info Version", 3);

    if (!QuietMode) {
        errs() << "Total: " << totalFunctions << " functions, "
               << totalBBs << " basic blocks, "
               << totalInstructions << " instructions labeled\n";
    }

    // Write BB mapping to file if requested
    if (!MappingOutput.empty()) {
        std::ofstream outFile(MappingOutput.getValue());
        if (outFile.is_open()) {
            outFile << bbMapping.dump(2) << "\n";
            if (!QuietMode) {
                errs() << "Wrote BB mapping to " << MappingOutput << "\n";
            }
        } else {
            errs() << "Warning: Could not write BB mapping to " << MappingOutput << "\n";
        }
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
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                    if (Name == "assign-bb-debuginfo") {
                        MPM.addPass(bbdebuginfo::AssignBBDebugInfoPass());
                        return true;
                    }
                    return false;
                });
        }
    };
}

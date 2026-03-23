#include "AssignMIRBBDebugInfoPass.h"

#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"

#include "common/Logger.h"

#include <fstream>
#include <nlohmann/json.hpp>

using namespace llvm;

static cl::opt<std::string>
    MIRBBMappingOutput("mir-bb-mapping",
                       cl::desc("Output path for MIR BB index-to-name mapping JSON"), cl::init(""));

namespace checkpoint {

char AssignMIRBBDebugInfoPass::ID = 0;

AssignMIRBBDebugInfoPass::AssignMIRBBDebugInfoPass() : MachineFunctionPass(ID) {}

bool AssignMIRBBDebugInfoPass::runOnMachineFunction(MachineFunction &MF) {
    initLogging();

    Function &F = MF.getFunction();
    Module *M = F.getParent();
    LLVMContext &Ctx = M->getContext();

    // We need a DISubprogram to create DebugLocs. If the function already has one
    // (from the IR-level assign-bb-debuginfo pass), reuse it. Otherwise skip.
    DISubprogram *SP = F.getSubprogram();
    if (!SP) {
        PLOGW << "Function '" << F.getName()
              << "' has no DISubprogram — skipping MIR BB debug info assignment. "
              << "Run assign-bb-debuginfo (IR pass) first.";
        return false;
    }

    // Build mapping and reassign DebugLocs.
    // MIR BB indices are 1-based in DWARF (line 0 means "no location").
    nlohmann::json funcMapping;
    unsigned bbIndex = 1;
    unsigned totalLabeled = 0;

    for (MachineBasicBlock &MBB : MF) {
        // Create a DebugLoc with line = bbIndex, column = 0, scope = SP
        DILocation *Loc = DILocation::get(Ctx, /*Line=*/bbIndex, /*Column=*/0, /*Scope=*/SP);
        DebugLoc DL(Loc);

        // Use a unique name per MIR BB: "mirbb" + MBB number.
        // We cannot use MBB.getName() because multiple MIR BBs can share
        // the same IR BB name (after codegen splitting), which would cause
        // bb-energy-analyzer to overwrite earlier entries in the JSON output.
        std::string bbName = "mirbb" + std::to_string(MBB.getNumber());
        funcMapping[std::to_string(bbIndex)] = bbName;

        unsigned labeledInBB = 0;
        for (MachineInstr &MI : MBB) {
            // Skip debug instructions — they don't produce machine code
            if (MI.isDebugInstr())
                continue;
            MI.setDebugLoc(DL);
            labeledInBB++;
        }

        totalLabeled += labeledInBB;
        bbIndex++;
    }

    PLOGI << "MIR BB debug info: labeled " << totalLabeled << " instructions across "
          << (bbIndex - 1) << " MIR BBs in '" << F.getName() << "'";

    // Append this function's mapping to the output file.
    // We accumulate across functions since runOnMachineFunction is called per function.
    if (!MIRBBMappingOutput.empty()) {
        // Read existing mapping (other functions may have been written already)
        nlohmann::json allMappings;
        {
            std::ifstream inFile(MIRBBMappingOutput.getValue());
            if (inFile.is_open() && inFile.peek() != std::ifstream::traits_type::eof()) {
                allMappings = nlohmann::json::parse(inFile, nullptr, false);
                if (allMappings.is_discarded())
                    allMappings = nlohmann::json::object();
            }
        }

        allMappings[F.getName().str()] = funcMapping;

        std::ofstream outFile(MIRBBMappingOutput.getValue());
        if (outFile.is_open()) {
            outFile << allMappings.dump(2) << "\n";
        } else {
            PLOGW << "Warning: Could not write MIR BB mapping to " << MIRBBMappingOutput;
        }
    }

    return true; // We modified DebugLocs
}

} // namespace checkpoint

// Forward-declare the generated initialization function
namespace llvm {
void initializeAssignMIRBBDebugInfoPassPass(PassRegistry &);
} // namespace llvm

using AssignMIRBBDebugInfoPass = checkpoint::AssignMIRBBDebugInfoPass;

INITIALIZE_PASS(AssignMIRBBDebugInfoPass, "assign-mir-bb-debuginfo", "Assign MIR BB Debug Info",
                false, false)

namespace {
struct MIRBBDebugInfoInitializer {
    MIRBBDebugInfoInitializer() {
        initializeAssignMIRBBDebugInfoPassPass(*PassRegistry::getPassRegistry());
    }
} MIRBBDebugInfoX;
} // namespace

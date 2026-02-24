#include "schematic/SchematicPass.h"

#include "llvm/IR/Function.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace checkpoint {

PreservedAnalyses SchematicPass::run(Function &F,
                                     FunctionAnalysisManager &AM) {
    errs() << "SCHEMATIC: skeleton pass (not yet implemented) on "
           << F.getName() << "\n";
    return PreservedAnalyses::all();
}

} // namespace checkpoint

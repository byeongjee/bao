#include "common/BBNaming.h"

#include "llvm/IR/BasicBlock.h"

namespace checkpoint {

void ensureBBNames(llvm::Function &F) {
    unsigned idx = 0;
    for (llvm::BasicBlock &BB : F) {
        if (!BB.hasName()) {
            BB.setName("bb." + llvm::Twine(idx));
        }
        idx++;
    }
}

} // namespace checkpoint

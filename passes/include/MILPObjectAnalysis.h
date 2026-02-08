#pragma once

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Function.h"

#include <cstdint>
#include <string>
#include <vector>

namespace llvm {
class CallBase;
class GlobalVariable;
class Module;
} // namespace llvm

namespace checkpoint {

struct MILPTrackedObject {
    llvm::Value *value = nullptr;
    std::string name;
    uint64_t sizeBytes = 0;
    bool isGlobal = false;
    bool pointerEscaped = false;
    bool aliasAmbiguous = false;
    std::string reason;
};

struct MILPObjectAnalysisResult {
    std::vector<MILPTrackedObject> vmCandidateObjects;
    std::vector<MILPTrackedObject> forcedNVMObjects;
    std::vector<MILPTrackedObject> excludedObjects;
};

/// Conservative object classification for phase-1 bring-up.
/// Any pointer-escaped or alias-ambiguous object is forced to NVM.
class MILPObjectAnalysis {
public:
    explicit MILPObjectAnalysis(llvm::Function &F);

    MILPObjectAnalysisResult analyze();

private:
    llvm::Function &F_;
    llvm::Module &M_;

    bool valueEscapes(llvm::Value *ptrValue, bool &aliasAmbiguous);
    bool valueEscapesRecursive(llvm::Value *ptrValue,
                               llvm::SmallPtrSetImpl<llvm::Value*> &visited,
                               bool &aliasAmbiguous);

    bool isNonEscapingIntrinsic(const llvm::CallBase &call) const;
    bool shouldSkipGlobal(const llvm::GlobalVariable &GV) const;
    uint64_t getObjectSizeBytes(llvm::Value *value) const;
    std::string getObjectName(const llvm::Value *value) const;
};

} // namespace checkpoint

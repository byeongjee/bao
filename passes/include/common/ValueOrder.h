#ifndef CHECKPOINT_VALUE_ORDER_H
#define CHECKPOINT_VALUE_ORDER_H

#include "common/BlockUtils.h"

#include "llvm/IR/Argument.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <string>
#include <vector>

namespace checkpoint {

/// Build a stable, semantic sort key for LLVM Values.
///
/// Raw Value* addresses vary across process runs, so any ordering derived from
/// pointer values makes the MILP model assembly and instrumentation unstable.
/// This key is based on IR identity instead.
inline std::string getStableValueOrderKey(const llvm::Value *V) {
    if (!V)
        return "<null>";

    std::string key;
    llvm::raw_string_ostream os(key);

    if (auto *GV = llvm::dyn_cast<llvm::GlobalValue>(V)) {
        os << "g:" << GV->getName();
        return os.str();
    }

    if (auto *Arg = llvm::dyn_cast<llvm::Argument>(V)) {
        os << "a:" << Arg->getParent()->getName() << ":";
        if (Arg->hasName())
            os << Arg->getName();
        else
            os << Arg->getArgNo();
        return os.str();
    }

    if (auto *I = llvm::dyn_cast<llvm::Instruction>(V)) {
        os << "i:" << I->getFunction()->getName() << ":"
           << getBlockName(*I->getParent(), *I->getFunction()) << ":";
    } else {
        os << "v:";
    }

    V->printAsOperand(os, false);
    return os.str();
}

template <typename T> inline void stableSortValues(std::vector<T *> &values) {
    std::sort(values.begin(), values.end(), [](const T *lhs, const T *rhs) {
        return getStableValueOrderKey(lhs) < getStableValueOrderKey(rhs);
    });
}

template <typename T> inline void stableSortAndUniqueValues(std::vector<T *> &values) {
    stableSortValues(values);
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

} // namespace checkpoint

#endif // CHECKPOINT_VALUE_ORDER_H

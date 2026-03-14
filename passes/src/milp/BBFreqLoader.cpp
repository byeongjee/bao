#include "milp/BBFreqLoader.h"

#include "common/Logger.h"

#include "llvm/Support/raw_ostream.h"

#define JSON_NOEXCEPTION
#include <fstream>
#include <nlohmann/json.hpp>

namespace checkpoint {

std::optional<BBFreqLoader> BBFreqLoader::load(const std::string &jsonPath, llvm::Function &F) {
    std::ifstream file(jsonPath);
    if (!file.is_open()) {
        PLOGE << "BBFreqLoader: cannot open file: " << jsonPath;
        return std::nullopt;
    }

    nlohmann::json root = nlohmann::json::parse(file, nullptr, false);
    if (root.is_discarded()) {
        PLOGE << "BBFreqLoader: JSON parse error in: " << jsonPath;
        return std::nullopt;
    }

    std::string funcName = F.getName().str();
    if (!root.contains(funcName)) {
        PLOGE << "BBFreqLoader: function '" << funcName << "' not found in " << jsonPath;
        return std::nullopt;
    }

    const auto &funcObj = root[funcName];
    if (!funcObj.is_object()) {
        PLOGE << "BBFreqLoader: expected object for function '" << funcName << "'";
        return std::nullopt;
    }

    // Build a name-to-BB map for the function
    llvm::DenseMap<llvm::StringRef, llvm::BasicBlock *> nameMap;
    for (llvm::BasicBlock &BB : F) {
        if (BB.hasName())
            nameMap[BB.getName()] = &BB;
    }

    BBFreqLoader loader;
    for (auto it = funcObj.begin(); it != funcObj.end(); ++it) {
        std::string bbName = it.key();
        if (!it.value().is_number()) {
            PLOGE << "BBFreqLoader: non-numeric count for BB '" << bbName << "' in function '"
                  << funcName << "'";
            return std::nullopt;
        }
        uint64_t count = it.value().get<uint64_t>();

        auto mapIt = nameMap.find(llvm::StringRef(bbName));
        if (mapIt == nameMap.end()) {
            PLOGE << "BBFreqLoader: BB '" << bbName << "' not found in function '" << funcName
                  << "'";
            return std::nullopt;
        }

        loader.counts_[mapIt->second] = count;
    }

    return loader;
}

std::optional<uint64_t> BBFreqLoader::getBlockCount(const llvm::BasicBlock *BB) const {
    auto it = counts_.find(BB);
    if (it != counts_.end())
        return it->second;
    return std::nullopt;
}

} // namespace checkpoint

#include "schematic/SchematicBlock.h"

namespace checkpoint {

SchematicBlock::SchematicBlock(llvm::BasicBlock *bb) : bb_(bb), name_(bb->getName().str()) {}

SchematicBlock::SchematicBlock(std::string name) : bb_(nullptr), name_(std::move(name)) {}

SchematicBlock *SchematicGraph::getOrCreate(llvm::BasicBlock *bb) {
    auto &entry = realBlocks_[bb];
    if (!entry)
        entry = std::make_unique<SchematicBlock>(bb);
    return entry.get();
}

SchematicBlock *SchematicGraph::createSynthetic(const std::string &name) {
    syntheticBlocks_.push_back(std::make_unique<SchematicBlock>(name));
    return syntheticBlocks_.back().get();
}

void SchematicGraph::addTraceEdges(const std::vector<SchematicBlock *> &trace) {
    for (unsigned i = 0; i + 1 < trace.size(); ++i) {
        trace[i]->addSuccessor(trace[i + 1]);
        trace[i + 1]->addPredecessor(trace[i]);
    }
}

} // namespace checkpoint

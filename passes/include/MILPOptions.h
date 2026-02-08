#pragma once

#include "llvm/Support/CommandLine.h"

#include <string>

namespace checkpoint {

extern llvm::cl::opt<std::string> EnergyConfigOpt;
extern llvm::cl::opt<std::string> MILPConfigOpt;
extern llvm::cl::opt<std::string> CheckpointAlgorithmOpt;

} // namespace checkpoint

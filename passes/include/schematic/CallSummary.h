#pragma once

#include "schematic/SchematicSolution.h"

#include "llvm/IR/Value.h"

#include <map>

namespace checkpoint {

/// Summary of a solved callee, folded into each caller call site by the
/// inter-procedural driver. `sf*` = callee first block (START_Func), `ef*` =
/// callee last block (END_Func). These are exactly the values that the reference
/// update_function_basic_blocks reads from callee_cfg.first_bb / last_bb
/// (schematic.py:106-131). Captured before the per-function FuncScopeGuard
/// erases the synthetic boundary nodes.
struct CallSummary {
    bool feasible = false;
    bool checkpointInFunction = false; // callee has a non-DISABLED checkpoint (=> VIRTUAL)
    double sfEToLeave = 0.0;
    double sfELeft = 0.0;
    double efEToLeave = 0.0;
    double efELeft = 0.0;
    RegionAllocation sfAllocation;
    RegionAllocation efAllocation;
    std::map<llvm::Value *, Placement> sfPlacements;
    std::map<llvm::Value *, Placement> efPlacements;
};

} // namespace checkpoint
